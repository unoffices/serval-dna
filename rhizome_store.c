/*
Serval DNA Rhizome storage
Copyright (C) 2013 Serval Project Inc.
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#ifdef HAVE_SYS_STATVFS_H
#  include <sys/statvfs.h>
#else
#  if defined(HAVE_SYS_STAT_H) && defined(HAVE_SYS_VFS_H)
#    include <sys/stat.h>
#    include <sys/vfs.h>
#    define statvfs statfs
#  endif
#endif
#include "serval.h"
#include "rhizome.h"
#include "conf.h"
#include "str.h"
#include "numeric_str.h"
#include "debug.h"

#define RHIZOME_BUFFER_MAXIMUM_SIZE (1024*1024)

uint64_t rhizome_copy_file_to_blob(int fd, uint64_t id, size_t size);

enum rhizome_payload_status rhizome_exists(const rhizome_filehash_t *hashp)
{
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  uint64_t gotfile = 0;
  int stepcode = sqlite_exec_uint64_retry(&retry, &gotfile, "SELECT COUNT(*) FROM FILES WHERE id = ? and datavalid = 1;",
	RHIZOME_FILEHASH_T, hashp, END);
  if (sqlite_code_busy(stepcode))
    return RHIZOME_PAYLOAD_STATUS_BUSY;
  if (!sqlite_code_ok(stepcode))
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  if (gotfile==0)
    return RHIZOME_PAYLOAD_STATUS_NEW;

  char blob_path[1024];
  if (FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%s", RHIZOME_BLOB_SUBDIR, alloca_tohex_rhizome_filehash_t(*hashp))){
    struct stat st;
    if (stat(blob_path, &st) == 0)
      return RHIZOME_PAYLOAD_STATUS_STORED;
  }

  uint64_t blob_rowid = 0;
  stepcode = sqlite_exec_uint64_retry(&retry, &blob_rowid,
	"SELECT rowid "
	"FROM FILEBLOBS "
	"WHERE id = ?", RHIZOME_FILEHASH_T, hashp, END);

  if (sqlite_code_busy(stepcode))
    return RHIZOME_PAYLOAD_STATUS_BUSY;
  if (!sqlite_code_ok(stepcode))
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  if (blob_rowid!=0)
    return RHIZOME_PAYLOAD_STATUS_STORED;
  return RHIZOME_PAYLOAD_STATUS_NEW;
}

/* Creates a row in the FILEBLOBS table and return the ROWID.  Returns 0 if unsuccessful (error
 * logged).
 */
static uint64_t rhizome_create_fileblob(sqlite_retry_state *retry, uint64_t id, size_t size)
{
  if (sqlite_exec_void_retry(
      retry,
      "INSERT OR REPLACE INTO FILEBLOBS(id,data) VALUES(?,?)",
      UINT64_TOSTR, id,
      ZEROBLOB, (int)size,
      END) == -1
  ) {
    WHYF("Failed to create blob, size=%zu, id=%"PRIu64, size, id);
    return 0;
  }
  uint64_t rowid = sqlite3_last_insert_rowid(rhizome_db);
  DEBUGF(rhizome_store, "Inserted fileblob rowid=%"PRId64" for id='%"PRIu64"'", rowid, id);
  return rowid;
}

static int rhizome_delete_external(const char *id)
{
  // attempt to remove any external blob & partial hash file
  char blob_path[1024];
  if (FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%s", RHIZOME_HASH_SUBDIR, id))
    unlink(blob_path);
  if (!FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%s", RHIZOME_BLOB_SUBDIR, id))
    return -1;
  if (unlink(blob_path) == -1) {
    if (errno != ENOENT)
      return WHYF_perror("unlink(%s)", alloca_str_toprint(blob_path));
    return 1;
  }
  DEBUGF(rhizome_store, "Deleted blob file %s", blob_path);
  return 0;
}

static int rhizome_delete_file_id_retry(sqlite_retry_state *retry, const char *id)
{
  int ret = 0;
  rhizome_delete_external(id);
  sqlite3_stmt *statement = sqlite_prepare_bind(retry, "DELETE FROM fileblobs WHERE id = ?", STATIC_TEXT, id, END);
  if (!statement || sqlite_exec_retry(retry, statement) == -1)
    ret = -1;
  statement = sqlite_prepare_bind(retry, "DELETE FROM files WHERE id = ?", STATIC_TEXT, id, END);
  if (!statement || sqlite_exec_retry(retry, statement) == -1)
    ret = -1;
  return ret == -1 ? -1 : sqlite3_changes(rhizome_db) ? 0 : 1;
}

static int rhizome_delete_payload_retry(sqlite_retry_state *retry, const rhizome_bid_t *bidp)
{
  strbuf fh = strbuf_alloca(RHIZOME_FILEHASH_STRLEN + 1);
  int rows = sqlite_exec_strbuf_retry(retry, fh, "SELECT filehash FROM manifests WHERE id = ?", RHIZOME_BID_T, bidp, END);
  if (rows == -1)
    return -1;
  if (rows && rhizome_delete_file_id_retry(retry, strbuf_str(fh)) == -1)
    return -1;
  return 0;
}

/* Remove a bundle's payload (file) from the database, given its manifest ID, leaving its manifest
 * untouched if present.
 *
 * Returns 0 if manifest is found, its payload is found and removed
 * Returns 1 if manifest or payload is not found
 * Returns -1 on error
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
int rhizome_delete_payload(const rhizome_bid_t *bidp)
{
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  return rhizome_delete_payload_retry(&retry, bidp);
}

int rhizome_delete_file_id(const char *id)
{
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  return rhizome_delete_file_id_retry(&retry, id);
}

/* Remove a file from the database, given its file hash.
 *
 * Returns 0 if file is found and removed
 * Returns 1 if file is not found
 * Returns -1 on error
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
int rhizome_delete_file(const rhizome_filehash_t *hashp)
{
  return rhizome_delete_file_id(alloca_tohex_rhizome_filehash_t(*hashp));
}

static uint64_t store_get_free_space()
{
  const char *fake_space = getenv("SERVALD_FREE_SPACE");
  uint64_t space = UINT64_MAX;
  if (fake_space)
    space = atol(fake_space);
#if defined(HAVE_SYS_STATVFS_H) || (defined(HAVE_SYS_STAT_H) && defined(HAVE_SYS_VFS_H))
  else {
    char store_path[1024];
    if (FORMF_RHIZOME_STORE_PATH(store_path, "rhizome.db")) {
      struct statvfs stats;
      if (statvfs(store_path, &stats)==-1)
	WARNF_perror("statvfs(%s)", store_path);
      else
	space = stats.f_frsize * (uint64_t)stats.f_bavail;
    }
  }
#endif
  if (IF_DEBUG(rhizome)) {
    // Automated tests depend on this message; do not alter.
    DEBUGF(rhizome, "RHIZOME SPACE FREE bytes=%"PRIu64" (%sB)", space, alloca_double_scaled_binary(space));
  }
  return space;
}

static uint64_t store_space_limit(uint64_t current_size)
{
  uint64_t limit = config.rhizome.database_size;
  
  if (config.rhizome.min_free_space!=0){
    uint64_t free_space = store_get_free_space();
    if (free_space < config.rhizome.min_free_space){
      if (current_size + free_space < config.rhizome.min_free_space)
	limit = 0;
      else
	limit = current_size + free_space - config.rhizome.min_free_space;
    }
  }
  return limit;
}

// TODO readonly version?
static enum rhizome_payload_status store_make_space(uint64_t bytes, struct rhizome_cleanup_report *report)
{
  uint64_t external_bytes=0;
  uint64_t db_page_size=0;
  uint64_t db_page_count=0;
  uint64_t db_free_page_count=0;
  
  // No limit?
  if (config.rhizome.database_size==UINT64_MAX && config.rhizome.min_free_space==0)
    return RHIZOME_PAYLOAD_STATUS_NEW;
  
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  int stepcode = sqlite_exec_uint64_retry(&retry, &db_page_size, "PRAGMA page_size;", END);
  if (sqlite_code_ok(stepcode))
    stepcode = sqlite_exec_uint64_retry(&retry, &db_page_count, "PRAGMA page_count;", END);
  if (sqlite_code_ok(stepcode))
    stepcode = sqlite_exec_uint64_retry(&retry, &db_free_page_count, "PRAGMA freelist_count;", END);
  if (sqlite_code_ok(stepcode))
    // TODO index and/or cache result?
    stepcode = sqlite_exec_uint64_retry(&retry, &external_bytes,
	  "SELECT SUM(length) "
	  "FROM FILES  "
	  "WHERE NOT EXISTS( "
	    "SELECT 1  "
	    "FROM FILEBLOBS "
	    "WHERE FILES.ID = FILEBLOBS.ID "
	  ");", END);

  if (sqlite_code_busy(stepcode))
    return RHIZOME_PAYLOAD_STATUS_BUSY;
  if (!sqlite_code_ok(stepcode))
    return RHIZOME_PAYLOAD_STATUS_ERROR;

  uint64_t db_used = external_bytes + db_page_size * (db_page_count - db_free_page_count);
  const uint64_t limit = store_space_limit(db_used);

  // Automated tests depend on this message; do not alter.
  DEBUGF(rhizome, "RHIZOME SPACE USED bytes=%"PRIu64" (%sB), LIMIT bytes=%"PRIu64" (%sB)",
      db_used, alloca_double_scaled_binary(db_used),
      limit, alloca_double_scaled_binary(limit));
  
  if (bytes && bytes >= limit){
    DEBUGF(rhizome, "Not enough space for %"PRIu64". Used; %"PRIu64" = %"PRIu64" + %"PRIu64" * (%"PRIu64" - %"PRIu64"), Limit; %"PRIu64, 
	   bytes, db_used, external_bytes, db_page_size, db_page_count, db_free_page_count, limit);
    return RHIZOME_PAYLOAD_STATUS_TOO_BIG;
  }
  
  // vacuum database pages if more than 1/4 of the db is free or we're already over the limit
  if (db_free_page_count > (db_page_count>>2)+1 || external_bytes + db_page_size * db_page_count > limit)
    rhizome_vacuum_db(&retry);
  
  // If there is enough space, do nothing
  if (db_used + bytes <= limit)
    return RHIZOME_PAYLOAD_STATUS_NEW;
  
  // penalise new things by 10 minutes to reduce churn
  time_ms_t cost = gettime_ms() - 60000 - bytes;
  
  // query files by age, penalise larger files so they are removed earlier
  sqlite3_stmt *statement = sqlite_prepare_bind(&retry,
      "SELECT id, length, inserttime FROM FILES ORDER BY (inserttime - length)",
      END);
  if (!statement)
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  
  while (db_used + bytes > limit && (stepcode=sqlite_step_retry(&retry, statement)) == SQLITE_ROW) {
    const char *id=(const char *) sqlite3_column_text(statement, 0);
    uint64_t length = sqlite3_column_int(statement, 1);
    time_ms_t inserttime = sqlite3_column_int64(statement, 2);
    
    time_ms_t cost_existing = inserttime - length;
    
    DEBUGF(rhizome, "Considering dropping file %s, size %"PRId64" cost %"PRId64" vs %"PRId64" to add %"PRId64" new bytes", 
	   id, length, cost, cost_existing, bytes);
    // don't allow the new file, we've got more important things to store
    if (bytes && cost < cost_existing)
      break;
    
    // drop the existing content and recalculate used space
    if (rhizome_delete_external(id)==0)
      external_bytes -= length;

    int rowcount=0;
    sqlite3_stmt *s = sqlite_prepare_bind(&retry, "DELETE FROM fileblobs WHERE id = ?", STATIC_TEXT, id, END);
    if (s && !sqlite_code_ok(stepcode = sqlite_exec_code_retry(&retry, s, &rowcount)))
      break;

    s = sqlite_prepare_bind(&retry, "DELETE FROM files WHERE id = ?", STATIC_TEXT, id, END);
    if (s && !sqlite_code_ok(stepcode = sqlite_exec_code_retry(&retry, s, &rowcount)))
      break;

    if (!sqlite_code_ok(stepcode = sqlite_exec_uint64_retry(&retry, &db_page_count, "PRAGMA page_count;", END)))
      break;
    if (!sqlite_code_ok(stepcode = sqlite_exec_uint64_retry(&retry, &db_free_page_count, "PRAGMA freelist_count;", END)))
      break;

    if (report)
      report->deleted_expired_files++;
    db_used = external_bytes + db_page_size * (db_page_count - db_free_page_count);
  }
  sqlite3_finalize(statement);

  if (sqlite_code_busy(stepcode))
    return RHIZOME_PAYLOAD_STATUS_BUSY;
  if (!sqlite_code_ok(stepcode))
    return RHIZOME_PAYLOAD_STATUS_ERROR;

  rhizome_vacuum_db(&retry);

  if (db_used + bytes <= limit)
    return RHIZOME_PAYLOAD_STATUS_NEW;

  DEBUGF(rhizome, "Not enough space for %"PRIu64". Used; %"PRIu64" = %"PRIu64" + %"PRIu64" * (%"PRIu64" - %"PRIu64"), Limit; %"PRIu64,
	 bytes, db_used, external_bytes, db_page_size, db_page_count, db_free_page_count, limit);

  return RHIZOME_PAYLOAD_STATUS_EVICTED;
}

int rhizome_store_cleanup(struct rhizome_cleanup_report *report)
{
  return store_make_space(0, report);
}

enum rhizome_payload_status rhizome_open_write(struct rhizome_write *write, const rhizome_filehash_t *expectedHashp, uint64_t file_length)
{
  DEBUGF(rhizome_store, "file_length=%"PRIu64, file_length);

  if (file_length == 0)
    return RHIZOME_PAYLOAD_STATUS_EMPTY;

  write->blob_fd=-1;
  write->sql_blob=NULL;
  
  if (expectedHashp){
    if (rhizome_exists(expectedHashp) == RHIZOME_PAYLOAD_STATUS_STORED)
      return RHIZOME_PAYLOAD_STATUS_STORED;
    write->id = *expectedHashp;
    write->id_known=1;
  }else{
    write->id_known=0;
  }
  
  if (file_length!=RHIZOME_SIZE_UNSET){
    enum rhizome_payload_status status = store_make_space(file_length, NULL);
    if (status != RHIZOME_PAYLOAD_STATUS_NEW)
      return status;
  }
  
  static unsigned id=0;
  write->temp_id = (getpid()<<16) + id++;
  
  write->file_length = file_length;
  write->file_offset = 0;
  write->written_offset = 0;
  crypto_hash_sha512_init(&write->sha512_context);
  return RHIZOME_PAYLOAD_STATUS_NEW;
}

/* blob_open / close will lock the database, this is bad for other processes that might attempt to 
 * use it at the same time. However, opening a blob has about O(n^2) performance. 
 * */

// encrypt and hash data, data buffers must be passed in file order.
static int prepare_data(struct rhizome_write *write_state, uint8_t *buffer, size_t data_size)
{
  if (data_size <= 0)
    return WHY("No content supplied");
    
  /* Make sure we aren't being asked to write more data than we expected */
  if (   write_state->file_length != RHIZOME_SIZE_UNSET
      && write_state->file_offset + data_size > write_state->file_length
  )
    return WHYF("Too much content supplied, %"PRIu64" + %zu > %"PRIu64,
		write_state->file_offset, data_size, write_state->file_length);

  if (write_state->crypt){
    if (rhizome_crypt_xor_block(
	  buffer, data_size, 
	  write_state->file_offset + write_state->tail, 
	  write_state->key, write_state->nonce))
      return -1;
  }
  
  crypto_hash_sha512_update(&write_state->sha512_context, buffer, data_size);
  write_state->file_offset+=data_size;
  
  DEBUGF(rhizome_store, "Processed %"PRIu64" of %"PRIu64, write_state->file_offset, write_state->file_length);
  return 0;
}

// open database locks
static int write_get_lock(struct rhizome_write *write_state)
{
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  
  if (write_state->blob_fd != -1 || write_state->sql_blob)
    return 0;
  
  if (write_state->file_length == RHIZOME_SIZE_UNSET || 
    write_state->file_length > config.rhizome.max_blob_size){
    char blob_path[1024];
    if (!FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%"PRIu64, RHIZOME_BLOB_SUBDIR, write_state->temp_id))
      return -1;
    DEBUGF(rhizome_store, "Attempting to put blob for id='%"PRIu64"' in %s", write_state->temp_id, blob_path);
    if ((write_state->blob_fd = open(blob_path, O_CREAT | O_TRUNC | O_RDWR, 0664)) == -1) {
      WHYF("Failed to create payload file, id='%"PRIu64"'", write_state->temp_id);
      return -1;
    }
    DEBUGF(rhizome_store, "Writing to new blob file %s (fd=%d)", blob_path, write_state->blob_fd);
  }else{
    // use an explicit transaction so we can delay I/O failures until COMMIT so they can be retried.
    if (sqlite_exec_void_retry(&retry, "BEGIN TRANSACTION;", END) == -1)
      return -1;
    if (write_state->blob_rowid == 0){
      write_state->blob_rowid = rhizome_create_fileblob(&retry, write_state->temp_id, write_state->file_length);
      if (write_state->blob_rowid == 0)
	goto fail;
    }
    if (sqlite_blob_open_retry(&retry, "main", "FILEBLOBS", "data", write_state->blob_rowid, 1 /* read/write */, &write_state->sql_blob) == -1)
      goto fail;
  }
  return 0;

fail:
  sqlite_exec_void_retry(&retry, "ROLLBACK;", END);
  return -1;
}

// write data to disk
static int write_data(struct rhizome_write *write_state, uint64_t file_offset, uint8_t *buffer, size_t data_size)
{
  DEBUGF(rhizome_store, "write_state->file_length=%"PRIu64" file_offset=%"PRIu64, write_state->file_length, file_offset);

  if (data_size<=0)
    return 0;
  
  if (file_offset != write_state->written_offset)
    WARNF("Writing file data out of order! [%"PRId64",%"PRId64"]", file_offset, write_state->written_offset);
    
  if (write_state->blob_fd != -1) {
    size_t ofs = 0;
    // keep trying until all of the data is written.
    if (lseek64(write_state->blob_fd, (off64_t) file_offset, SEEK_SET) == -1)
      return WHYF_perror("lseek64(%d,%"PRIu64",SEEK_SET)", write_state->blob_fd, file_offset);
    while (ofs < data_size){
      ssize_t r = write(write_state->blob_fd, buffer + ofs, (size_t)(data_size - ofs));
      if (r == -1)
	return WHY_perror("write");
      DEBUGF(rhizome_store, "Wrote %zd bytes to fd %d", (size_t)r, write_state->blob_fd);
      ofs += (size_t)r;
    }
  }else{
    if (!write_state->sql_blob)
      return WHY("Must call write_get_lock() before write_data()");
    sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
    if (sqlite_blob_write_retry(&retry, write_state->sql_blob, buffer, data_size, file_offset) == -1)
      return -1;
  }
  
  write_state->written_offset = file_offset + data_size;
  
  DEBUGF(rhizome_store, "Wrote %"PRIu64" of %"PRIu64, file_offset + data_size, write_state->file_length);
  return 0;
}

// close database locks
static int write_release_lock(struct rhizome_write *write_state)
{
  int ret=0;
  if (write_state->sql_blob){
    ret = sqlite_blob_close(write_state->sql_blob);
    sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
    if (sqlite_exec_void_retry(&retry, "COMMIT;", END) == -1){
      sqlite_exec_void_retry(&retry, "ROLLBACK;", END);
      ret=-1;
    }
    write_state->sql_blob=NULL;
  }
  return ret;
}

// Write data buffers in any order, the data will be cached and streamed into the database in file order. 
// Though there is an upper bound on the amount of cached data
int rhizome_random_write(struct rhizome_write *write_state, uint64_t offset, uint8_t *buffer, size_t data_size)
{
  DEBUGF(rhizome_store, "write_state->file_length=%"PRIu64" offset=%"PRIu64, write_state->file_length, offset);
  if (   write_state->file_length != RHIZOME_SIZE_UNSET
      && offset >= write_state->file_length)
    return 0;
    
  if (   write_state->file_length != RHIZOME_SIZE_UNSET
      && offset + data_size > write_state->file_length)
    data_size = write_state->file_length - offset;
  
  struct rhizome_write_buffer **ptr = &write_state->buffer_list;
  int ret=0;
  int should_write = 0;
  
  // if we are writing to a file, or already have the sql blob open, or are finishing, write as much
  // as we can.
  if (write_state->blob_fd != -1 || 
    write_state->sql_blob || 
    buffer == NULL ||
    write_state->file_length > config.rhizome.max_blob_size ||
    write_state->file_offset > config.rhizome.max_blob_size) {
    should_write = 1;
    DEBUGF(rhizome_store, "Attempting to write (fd=%d, blob=%p, buffer=%p, len=%"PRId64", offset=%"PRId64")",
	   write_state->blob_fd, write_state->sql_blob, buffer, 
	   write_state->file_length, write_state->file_offset);
  } else {
    // cache up to RHIZOME_BUFFER_MAXIMUM_SIZE or file length before attempting to write everything in one go.
    // (Not perfect if the range overlaps)
    uint64_t new_size = write_state->written_offset + write_state->buffer_size + data_size;
    if (   (write_state->file_length != RHIZOME_SIZE_UNSET && new_size >= write_state->file_length)
	|| new_size >= RHIZOME_BUFFER_MAXIMUM_SIZE
    )
      should_write = 1;
  }
  uint64_t last_offset = write_state->written_offset;
  
  while(1){
    
    // can we process this existing data block now?
    if (*ptr && (*ptr)->offset == write_state->file_offset){
      if (prepare_data(write_state, (*ptr)->data, (*ptr)->data_size)){
	ret=-1;
	break;
      }
    }
    
    // if existing data should be written, do so now
    if (should_write && *ptr && (*ptr)->offset == write_state->written_offset){
      struct rhizome_write_buffer *n=*ptr;

      if ( write_get_lock(write_state)
	|| write_data(write_state, n->offset, n->data, n->data_size)){
	// ignore transient write errors, eg db locks
	should_write = 0;
	continue;
      }

      *ptr=n->_next;
      write_state->buffer_size-=n->data_size;
      last_offset = n->offset + n->data_size;
      free(n);
      continue;
    }
    
    // skip over incoming data that we've already received
    if (offset < last_offset){
      uint64_t delta = last_offset - offset;
      if (delta >= data_size)
	break;
      data_size -= delta;
      offset+=delta;
      buffer+=delta;
    }
    
    // no new data? we can just stop now.
    if (data_size<=0)
      break;
    
    if (!*ptr || offset < (*ptr)->offset){
      // found the insert position in the list
      size_t size = data_size;
      
      // allow for buffers to overlap, we may need to split the incoming buffer into multiple pieces.
      if (*ptr && offset+size > (*ptr)->offset)
	size = (*ptr)->offset - offset;
	
      // should we process the incoming data block now?
      if (offset == write_state->file_offset){
	if (prepare_data(write_state, buffer, size)){
	  ret=-1;
	  break;
	}
      }
      
      if (should_write && offset == write_state->written_offset){

	if ( write_get_lock(write_state)
	  || write_data(write_state, offset, buffer, size)){
	  should_write = 0;
	  continue;
	}

	// we need to go around the loop again to re-test if *ptr can now be written

      }else{
	// impose a limit on the total amount of cached data
	if (write_state->buffer_size + size > RHIZOME_BUFFER_MAXIMUM_SIZE)
	  size = RHIZOME_BUFFER_MAXIMUM_SIZE - write_state->buffer_size;
	if (size<=0)
	  break;
	  
	DEBUGF(rhizome_store, "Caching block @%"PRId64", %zu", offset, size);
	struct rhizome_write_buffer *i = emalloc(size + sizeof(struct rhizome_write_buffer));
	if (!i){
	  ret=-1;
	  break;
	}
	i->offset = offset;
	i->buffer_size = i->data_size = size;
	bcopy(buffer, i->data, size);
	i->_next = *ptr;
	write_state->buffer_size += size;
	*ptr = i;
	// if there's any overlap of this buffer and the current one, we may need to add another buffer.
	ptr = &((*ptr)->_next);
      }
      data_size -= size;
      offset+=size;
      buffer+=size;
      continue;
    }
    
    last_offset = (*ptr)->offset + (*ptr)->data_size;
    ptr = &((*ptr)->_next);
  }
  if (write_release_lock(write_state))
    ret=-1;
  return ret;
}

int rhizome_write_buffer(struct rhizome_write *write_state, uint8_t *buffer, size_t data_size)
{
  return rhizome_random_write(write_state, write_state->file_offset, buffer, data_size);
}

/* If file_length is known, then expects file to be at least file_length in size, ignoring anything
 * longer than that.  Returns 0 if successful, -1 if error (logged).
 */
int rhizome_write_file(struct rhizome_write *write, const char *filename, off_t offset, uint64_t length)
{
  int fd = open(filename, O_RDONLY);
  if (fd == -1)
    return WHYF_perror("open(%s,O_RDONLY)", alloca_str_toprint(filename));
  unsigned char buffer[RHIZOME_CRYPT_PAGE_SIZE];
  int ret=0;
  if (offset){
    if (lseek(fd, offset, SEEK_SET)==-1)
      return WHYF_perror("lseek(%d,%zu,SEEK_SET)", fd, (unsigned long long)offset);
  }
  if (length == RHIZOME_SIZE_UNSET || length > write->file_length)
    length = write->file_length;
  while (length == RHIZOME_SIZE_UNSET || write->file_offset < length) {
    size_t size = sizeof buffer;
    if (length != RHIZOME_SIZE_UNSET && write->file_offset + size > length)
      size = length - write->file_offset;
    ssize_t r = read(fd, buffer, size);
    if (r == -1) {
      ret = WHYF_perror("read(%d,%p,%zu)", fd, buffer, size);
      break;
    }
    if (length != RHIZOME_SIZE_UNSET && (size_t) r != size) {
      ret = WHYF("file truncated - read(%d,%p,%zu) returned %zu", fd, buffer, size, (size_t) r);
      break;
    }
    if (r && rhizome_write_buffer(write, buffer, (size_t) r)) {
      ret = -1;
      break;
    }
    if ((size_t) r != size)
      break;
  }
  if (write_release_lock(write))
    ret = -1;
  close(fd);
  return ret;
}

int is_rhizome_write_open(const struct rhizome_write *write)
{
  return write->temp_id ? 1:0;
}

void rhizome_fail_write(struct rhizome_write *write)
{
  if (write->blob_fd != -1){
    DEBUGF(rhizome_store, "Closing and removing fd %d", write->blob_fd);
    close(write->blob_fd);
    write->blob_fd=-1;
    char blob_path[1024];
    if (FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%"PRIu64, RHIZOME_BLOB_SUBDIR, write->temp_id)){
      unlink(blob_path);
    }
  }
  write_release_lock(write);
  if (write->blob_rowid){
    sqlite_exec_void_loglevel(LOG_LEVEL_WARN, "DELETE FROM FILEBLOBS WHERE rowid = ?;", 
      INT64, write->blob_rowid, END);
  }
  while(write->buffer_list){
    struct rhizome_write_buffer *n=write->buffer_list;
    write->buffer_list=n->_next;
    free(n);
  }
  write->temp_id=0;
}

static int keep_hash(struct rhizome_write *write_state, struct crypto_hash_sha512_state *hash_state)
{
  char dest_path[1024];
  // capture the state of writing the file hash
  if (!FORMF_RHIZOME_STORE_PATH(dest_path, "%s/%s", RHIZOME_HASH_SUBDIR, alloca_tohex_rhizome_filehash_t(write_state->id)))
    return WHYF("Path too long?");
  int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    return WHYF_perror("Failed to create %s", dest_path);
  ssize_t w = write(fd, hash_state, sizeof *hash_state);
  close(fd);
  if (w != sizeof *hash_state)
    return WHYF("Failed to write hash state");
  DEBUGF(rhizome, "Preserved partial hash to %s", dest_path);
  return 1;
}

enum rhizome_payload_status rhizome_finish_write(struct rhizome_write *write)
{
  DEBUGF(rhizome_store, "blob_fd=%d file_offset=%"PRIu64"", write->blob_fd, write->file_offset);

  enum rhizome_payload_status status = RHIZOME_PAYLOAD_STATUS_NEW;
  
  // Once the whole file has been processed, we should finally know its length
  if (write->file_length == RHIZOME_SIZE_UNSET) {
    DEBUGF(rhizome_store, "Wrote %"PRIu64" bytes, set file_length", write->file_offset);
    write->file_length = write->file_offset;
    if (write->file_length == 0)
      status = RHIZOME_PAYLOAD_STATUS_EMPTY;
    else {
      status = store_make_space(write->file_length, NULL);
      if (status != RHIZOME_PAYLOAD_STATUS_NEW)
	goto failure;
    }
  }
  
  // flush out any remaining buffered pieces to disk
  if (write->buffer_list){
    if (rhizome_random_write(write, 0, NULL, 0) || write->buffer_list) {
      INFOF("Failed to flush write buffer");
      status = RHIZOME_PAYLOAD_STATUS_BUSY;
      goto failure;
    }
  }
  
  if (write->file_offset < write->file_length) {
    WHYF("Only wrote %"PRIu64" bytes, expected %"PRIu64, write->file_offset, write->file_length);
    status = RHIZOME_PAYLOAD_STATUS_WRONG_SIZE;
    goto failure;
  }
  assert(write->file_offset == write->file_length && write->written_offset == write->file_length);
  
  if (write->file_length == 0) {
    // whoops, no payload, don't store anything
    DEBUGF(rhizome_store, "Ignoring empty write");
    goto failure;
  }

  struct crypto_hash_sha512_state hash_state;
  if (write->journal)
    bcopy(&write->sha512_context, &hash_state, sizeof hash_state);

  rhizome_filehash_t hash_out;
  crypto_hash_sha512_final(&write->sha512_context, hash_out.binary);

  if (write->id_known) {
    if (cmp_rhizome_filehash_t(&write->id, &hash_out) != 0) {
      WARNF("expected filehash=%s, got %s", alloca_tohex_rhizome_filehash_t(write->id), alloca_tohex_rhizome_filehash_t(hash_out));
      write->id = hash_out;
      status = RHIZOME_PAYLOAD_STATUS_WRONG_HASH;
      goto failure;
    }
  } else
    write->id = hash_out;

  char blob_path[1024];
  if (!FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%"PRIu64, RHIZOME_BLOB_SUBDIR, write->temp_id)) {
    WHYF("Failed to generate external blob path");
    status = RHIZOME_PAYLOAD_STATUS_ERROR;
    goto failure;
  }
  // If the payload was written into an external blob (file) but is small enough to fit into a
  // SQLite blob, then copy it into a proper blob (this occurs if rhizome_open_write() was called
  // with file_length == RHIZOME_SIZE_UNSET) and max_blob_size > RHIZOME_BUFFER_MAXIMUM_SIZE.
  int external = 0;
  if (write->blob_fd != -1) {
    external = 1;
    if (write->file_length <= config.rhizome.max_blob_size) {
      DEBUGF(rhizome_store, "Copying %zu bytes from external file %s into blob, id=%"PRIu64, (size_t)write->file_offset, blob_path, write->temp_id);
      int ret = 0;
      if (lseek(write->blob_fd, 0, SEEK_SET) == (off_t) -1)
	ret = WHYF_perror("lseek(%d,0,SEEK_SET)", write->blob_fd);
      else if ((write->blob_rowid = rhizome_copy_file_to_blob(write->blob_fd, write->temp_id, (size_t)write->file_length)) == 0)
	ret = -1;
      if (ret == -1) {
	WHY("Failed to copy external file into blob; keeping external file");
      } else {
	external = 0;
	if (unlink(blob_path) == -1)
	  WARNF_perror("unlink(%s)", alloca_str_toprint(blob_path));
      }
    }
    DEBUGF(rhizome_store, "Closing fd=%d", write->blob_fd);
    close(write->blob_fd);
    write->blob_fd = -1;
  }
  if (write_release_lock(write)) {
    status = RHIZOME_PAYLOAD_STATUS_ERROR;
    goto failure;
  }

  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;

  if (sqlite_exec_void_retry(&retry, "BEGIN TRANSACTION;", END) == -1)
    goto dbfailure;

  // attempt the insert first
  time_ms_t now = gettime_ms();
  int rowcount, changes;
  int stepcode = sqlite_exec_changes_retry_loglevel(
	  LOG_LEVEL_INFO,
	  &retry, &rowcount, &changes,
	  "INSERT INTO FILES(id,length,datavalid,inserttime,last_verified) VALUES(?,?,1,?,?);",
	  RHIZOME_FILEHASH_T, &write->id,
	  INT64, write->file_length,
	  INT64, now,
	  INT64, now,
	  END
	);

  if (stepcode == SQLITE_CONSTRAINT){
    // we've already got that payload, delete the new copy
    if (write->blob_rowid){
      sqlite_exec_void_retry_loglevel(LOG_LEVEL_WARN, &retry, "DELETE FROM FILEBLOBS WHERE rowid = ?;",
	INT64, write->blob_rowid, END);
    }
    if (external){
      if (unlink(blob_path) == -1)
	WARNF_perror("unlink(%s)", alloca_str_toprint(blob_path));
    }
    DEBUGF(rhizome_store, "Payload id=%s already present, removed id='%"PRIu64"'", alloca_tohex_rhizome_filehash_t(write->id), write->temp_id);
    status = RHIZOME_PAYLOAD_STATUS_STORED;

  }else if(sqlite_code_ok(stepcode)){

    if (external) {
      char dest_path[1024];
      if (!FORMF_RHIZOME_STORE_PATH(dest_path, "%s/%s", RHIZOME_BLOB_SUBDIR, alloca_tohex_rhizome_filehash_t(write->id)))
	goto dbfailure;
      if (rename(blob_path, dest_path) == -1) {
	WHYF_perror("rename(%s, %s)", blob_path, dest_path);
	goto dbfailure;
      }
      DEBUGF(rhizome_store, "Renamed %s to %s", blob_path, dest_path);
      if (write->journal)
	keep_hash(write, &hash_state);
    }else{
      if (sqlite_exec_void_retry(
	    &retry,
	    "UPDATE FILEBLOBS SET id = ? WHERE rowid = ?",
	    RHIZOME_FILEHASH_T, &write->id,
	    INT64, write->blob_rowid,
	    END
	  ) == -1
	)
	  goto dbfailure;
    }
  }else
    goto dbfailure;

  if (sqlite_exec_void_retry(&retry, "COMMIT;", END) == -1)
    goto dbfailure;

  write->blob_rowid = 0;
  // A test case in tests/rhizomeprotocol depends on this debug message:
  if (status == RHIZOME_PAYLOAD_STATUS_NEW)
    DEBUGF(rhizome_store, "Stored file %s", alloca_tohex_rhizome_filehash_t(write->id));

  return status;

dbfailure:
  sqlite_exec_void_retry(&retry, "ROLLBACK;", END);
  status = RHIZOME_PAYLOAD_STATUS_ERROR;
failure:
  if (status != RHIZOME_PAYLOAD_STATUS_BUSY)
    rhizome_fail_write(write);
  return status;
}

/* Import the payload for an existing manifest with a known file size and hash.  Compute the hash of
 * the payload as it is imported, and when finished, check if the size and hash match the manifest.
 * If the import is successful and the size and hash match, return 0.  If the size or hash do not
 * match, return 1.  If there is an error reading the payload file or writing to the database,
 * return -1.
 */
enum rhizome_payload_status rhizome_import_payload_from_file(rhizome_manifest *m, const char *filepath)
{
  assert(m->filesize != RHIZOME_SIZE_UNSET);
  if (m->filesize == 0)
    return RHIZOME_PAYLOAD_STATUS_EMPTY;
  
  /* Import the file first, checking the hash as we go */
  struct rhizome_write write;
  bzero(&write, sizeof(write));
  
  enum rhizome_payload_status status = rhizome_open_write(&write, &m->filehash, m->filesize);
  if (status != RHIZOME_PAYLOAD_STATUS_NEW)
    return status;
  
  // file payload is not in the store yet
  if (rhizome_write_file(&write, filepath, 0, RHIZOME_SIZE_UNSET)){
    rhizome_fail_write(&write);
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  }

  status = rhizome_finish_write(&write);
  if (status == RHIZOME_PAYLOAD_STATUS_BUSY)
    rhizome_fail_write(&write);

  return status;
}

// store a whole payload from a single buffer
enum rhizome_payload_status rhizome_import_buffer(rhizome_manifest *m, uint8_t *buffer, size_t length)
{
  assert(m->filesize != RHIZOME_SIZE_UNSET);
  if (m->filesize == 0)
    return RHIZOME_PAYLOAD_STATUS_EMPTY;

  if (length != m->filesize) {
    WHYF("Expected %"PRIu64" bytes, got %zu", m->filesize, length);
    return RHIZOME_PAYLOAD_STATUS_WRONG_SIZE;
  }
  
  /* Import the file first, checking the hash as we go */
  struct rhizome_write write;
  bzero(&write, sizeof(write));
  
  enum rhizome_payload_status status = rhizome_open_write(&write, &m->filehash, m->filesize);
  if (status != RHIZOME_PAYLOAD_STATUS_NEW)
    return status;
  
  // file payload is not in the store yet
  if (rhizome_write_buffer(&write, buffer, length)){
    rhizome_fail_write(&write);
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  }
  
  status = rhizome_finish_write(&write);
  if (status == RHIZOME_PAYLOAD_STATUS_BUSY)
    rhizome_fail_write(&write);

  return status;
}

/* Checks the size of the file with the given path as a candidate payload for an existing manifest.
 * An empty path (zero length) is taken to mean empty payload (size = 0).  If the manifest's
 * 'filesize' is not yet set, then sets the manifest's 'filesize' to the size of the file and
 * returns 0.  Otherwise, if the file's size equals the 'filesize' in the manifest, return 0.  If
 * the file size does not match the manifest's 'filesize', returns 1.  If there is an error calling
 * stat(2) on the payload file (eg, file does not exist), returns -1.
 */
enum rhizome_payload_status rhizome_stat_payload_file(rhizome_manifest *m, const char *filepath)
{
  uint64_t size = 0;
  if (filepath[0]) {
    struct stat stat;
    if (lstat(filepath, &stat)) {
      WHYF_perror("lstat(%s)", alloca_str_toprint(filepath));
      return RHIZOME_PAYLOAD_STATUS_ERROR;
    }
    size = stat.st_size;
  }
  if (m->filesize == RHIZOME_SIZE_UNSET)
    rhizome_manifest_set_filesize(m, size);
  else if (size != m->filesize) {
    DEBUGF(rhizome_store, "payload file %s (size=%"PRIu64") does not match manifest %p filesize=%"PRIu64,
	   alloca_str_toprint(filepath), size, m, m->filesize);
    return RHIZOME_PAYLOAD_STATUS_WRONG_SIZE;
  }
  return size ? RHIZOME_PAYLOAD_STATUS_NEW : RHIZOME_PAYLOAD_STATUS_EMPTY;
}

static enum rhizome_payload_status rhizome_write_derive_key(rhizome_manifest *m, struct rhizome_write *write)
{
  if (m->payloadEncryption != PAYLOAD_ENCRYPTED)
    return RHIZOME_PAYLOAD_STATUS_NEW;

  // if the manifest specifies encryption, make sure we can generate the payload key and encrypt the
  // contents as we go
  if (!rhizome_derive_payload_key(m))
    return RHIZOME_PAYLOAD_STATUS_CRYPTO_FAIL;

  DEBUGF(rhizome_store, "Encrypting payload contents for bid=%s, version=%"PRIu64,
	 alloca_tohex_rhizome_bid_t(m->keypair.public_key), m->version);

  write->crypt=1;
  if (m->is_journal && m->tail > 0)
    write->tail = m->tail;

  bcopy(m->payloadKey, write->key, sizeof(write->key));
  bcopy(m->payloadNonce, write->nonce, sizeof(write->nonce));
  return RHIZOME_PAYLOAD_STATUS_NEW;
}

enum rhizome_payload_status rhizome_write_open_manifest(struct rhizome_write *write, rhizome_manifest *m)
{
  enum rhizome_payload_status status = rhizome_open_write(
	  write,
	  m->has_filehash ? &m->filehash : NULL,
	  m->filesize
	);
  if (status == RHIZOME_PAYLOAD_STATUS_NEW)
    status = rhizome_write_derive_key(m, write);
  return status;
}

// import a file for a new bundle with an unknown file hash
// update the manifest with the details of the file
enum rhizome_payload_status rhizome_store_payload_file(rhizome_manifest *m, const char *filepath)
{
  // Stream the file directly into the database, encrypting & hashing as we go.
  struct rhizome_write write;
  bzero(&write, sizeof(write));
  enum rhizome_payload_status status = rhizome_write_open_manifest(&write, m);
  int status_ok = 0;
  switch (status) {
    case RHIZOME_PAYLOAD_STATUS_EMPTY:
    case RHIZOME_PAYLOAD_STATUS_NEW:
      status_ok = 1;
      break;
    case RHIZOME_PAYLOAD_STATUS_BUSY:
    case RHIZOME_PAYLOAD_STATUS_STORED:
    case RHIZOME_PAYLOAD_STATUS_TOO_BIG:
    case RHIZOME_PAYLOAD_STATUS_EVICTED:
    case RHIZOME_PAYLOAD_STATUS_ERROR:
    case RHIZOME_PAYLOAD_STATUS_WRONG_SIZE:
    case RHIZOME_PAYLOAD_STATUS_WRONG_HASH:
    case RHIZOME_PAYLOAD_STATUS_CRYPTO_FAIL:
      return status;
  }
  if (!status_ok)
    FATALF("rhizome_write_open_manifest() returned status = %d", status);
  if (rhizome_write_file(&write, filepath, 0, RHIZOME_SIZE_UNSET) == -1)
    status = RHIZOME_PAYLOAD_STATUS_ERROR;
  else
    status = rhizome_finish_write(&write);
  return rhizome_finish_store(&write, m, status);
}

/* Returns RHIZOME_PAYLOAD_STATUS_STORED if file blob found
 * Returns RHIZOME_PAYLOAD_STATUS_NEW if not found
 * Returns RHIZOME_PAYLOAD_STATUS_ERROR if unexpected error
 */
enum rhizome_payload_status rhizome_open_read(struct rhizome_read *read, const rhizome_filehash_t *hashp)
{
  read->id = *hashp;
  read->blob_rowid = 0;
  read->blob_fd = -1;
  read->verified = 0;
  read->offset = 0;
  read->hash_offset = 0;

  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;

  int stepcode = sqlite_exec_uint64_retry(&retry, &read->length,"SELECT length FROM FILES WHERE id = ?",
    RHIZOME_FILEHASH_T, &read->id, END);
  if (sqlite_code_busy(stepcode))
    return RHIZOME_PAYLOAD_STATUS_BUSY;
  if (stepcode != SQLITE_ROW){
    if (sqlite_code_ok(stepcode))
      return RHIZOME_PAYLOAD_STATUS_NEW;
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  }

  assert(read->length>0);
  crypto_hash_sha512_init(&read->sha512_context);

  char blob_path[1024];
  if (FORMF_RHIZOME_STORE_PATH(blob_path, "%s/%s", RHIZOME_BLOB_SUBDIR, alloca_tohex_rhizome_filehash_t(read->id))){
    int fd = open(blob_path, O_RDONLY);
    DEBUGF(rhizome_store, "open(%s) = %d", alloca_str_toprint(blob_path), fd);
    if (fd == -1){
      if (errno!=ENOENT)
	WHYF_perror("open(%s)", alloca_str_toprint(blob_path));
    }else{
      off64_t pos = lseek64(fd, 0, SEEK_END);
      if (pos == -1)
	WHYF_perror("lseek64(%s,0,SEEK_END)", alloca_str_toprint(blob_path));
      if (read->length <= (uint64_t)pos){
	read->blob_fd = fd;
	DEBUGF(rhizome_store, "Opened stored file %s as fd %d, len %"PRIu64" (%"PRIu64")", blob_path, read->blob_fd, read->length, pos);
	return RHIZOME_PAYLOAD_STATUS_STORED;
      }
      DEBUGF(rhizome_store, "Ignoring file? %s fd %d, len %"PRIu64", seek %zd", blob_path, fd, read->length, pos);
      close(fd);
    }
  }

  stepcode = sqlite_exec_uint64_retry(&retry, &read->blob_rowid,
      "SELECT rowid "
      "FROM FILEBLOBS "
      "WHERE id = ?", RHIZOME_FILEHASH_T, &read->id, END);

  if (sqlite_code_busy(stepcode))
    return RHIZOME_PAYLOAD_STATUS_BUSY;
  
  if (!sqlite_code_ok(stepcode))
    return RHIZOME_PAYLOAD_STATUS_ERROR;

  if (stepcode == SQLITE_ROW){
    DEBUGF(rhizome_store, "Opened stored blob, rowid %d", read->blob_rowid);
    return RHIZOME_PAYLOAD_STATUS_STORED;
  }
  // database is inconsistent, clean it up
  rhizome_delete_file(&read->id);
  return RHIZOME_PAYLOAD_STATUS_NEW;
}

static ssize_t rhizome_read_retry(sqlite_retry_state *retry, struct rhizome_read *read_state, unsigned char *buffer, size_t bufsz)
{
  IN();
  if (read_state->blob_fd != -1) {
    assert(read_state->offset <= read_state->length);
    if (lseek64(read_state->blob_fd, (off64_t) read_state->offset, SEEK_SET) == -1)
      RETURN(WHYF_perror("lseek64(%d,%"PRIu64",SEEK_SET)", read_state->blob_fd, read_state->offset));
    if (bufsz + read_state->offset > read_state->length)
      bufsz = read_state->length - read_state->offset;
    if (bufsz == 0)
      RETURN(0);
    ssize_t rd = read(read_state->blob_fd, buffer, bufsz);
    if (rd == -1)
      RETURN(WHYF_perror("read(%d,%p,%zu)", read_state->blob_fd, buffer, bufsz));
    DEBUGF(rhizome_store, "Read %zu bytes from fd=%d @%"PRIx64, (size_t) rd, read_state->blob_fd, read_state->offset);
    RETURN(rd);
  }
  if (read_state->blob_rowid == 0)
    RETURN(WHY("blob not created"));
  sqlite3_blob *blob = NULL;
  if (sqlite_blob_open_retry(retry, "main", "FILEBLOBS", "data", read_state->blob_rowid, 0 /* read only */, &blob) == -1)
    RETURN(WHY("blob open failed"));
  assert(blob != NULL);
  assert(read_state->length == (uint64_t)sqlite3_blob_bytes(blob));
  // A NULL buffer skips the actual sqlite3_blob_read() call, which is useful just to work out
  // the length.
  size_t bytes_read = 0;
  if (buffer && bufsz && read_state->offset < read_state->length) {
    bytes_read = (size_t)(read_state->length - read_state->offset);
    if (bytes_read > bufsz)
      bytes_read = bufsz;
    assert(bytes_read > 0);
    int ret;
    do {
      ret = sqlite3_blob_read(blob, buffer, (int) bytes_read, read_state->offset);
    } while (sqlite_code_busy(ret) && sqlite_retry(retry, "sqlite3_blob_read"));
    if (ret != SQLITE_OK) {
      WHYF("sqlite3_blob_read() failed: %s", sqlite3_errmsg(rhizome_db));
      sqlite_blob_close(blob);
      RETURN(-1);
    }
  }
  sqlite_blob_close(blob);
  RETURN(bytes_read);
  OUT();
}

/* Read content from the store, hashing and decrypting as we go. 
 Random access is supported, but hashing requires all payload contents to be read sequentially. */
// returns the number of bytes read
ssize_t rhizome_read(struct rhizome_read *read_state, unsigned char *buffer, size_t buffer_length)
{
  IN();
  // hash check failed, just return an error
  if (read_state->verified == -1)
    RETURN(-1);

  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  ssize_t n = rhizome_read_retry(&retry, read_state, buffer, buffer_length);
  if (n == -1)
    RETURN(-1);
  size_t bytes_read = (size_t) n;

  // hash the payload as we go, but only if we happen to read the payload data in order
  if (read_state->hash_offset == read_state->offset && buffer && bytes_read>0){
    crypto_hash_sha512_update(&read_state->sha512_context, buffer, bytes_read);
    read_state->hash_offset += bytes_read;
    
    // if we hash everything and the hash doesn't match, we need to delete the payload
    if (read_state->hash_offset >= read_state->length){
      rhizome_filehash_t hash_out;
      crypto_hash_sha512_final(&read_state->sha512_context, hash_out.binary);
      if (cmp_rhizome_filehash_t(&read_state->id, &hash_out) != 0) {
	// hash failure, mark the payload as invalid
	read_state->verified = -1;
	RETURN(WHYF("Expected hash=%s, got %s", alloca_tohex_rhizome_filehash_t(read_state->id), alloca_tohex_rhizome_filehash_t(hash_out)));
      }else{
	// we read it, and it's good. Lets remember that (not fatal if the database is locked)
	read_state->verified = 1;
      }
    }
  }
  
  if (read_state->crypt && buffer && bytes_read>0){
    if(rhizome_crypt_xor_block(
	buffer, bytes_read, 
	read_state->offset + read_state->tail, 
	read_state->key, read_state->nonce)){
      RETURN(-1);
    }
  }
  read_state->offset += bytes_read;
  DEBUGF(rhizome_store, "read %zu bytes, read_state->offset=%"PRIu64, bytes_read, read_state->offset);
  RETURN(bytes_read);
  OUT();
}

/* Read len bytes from read->offset into data, using *buffer to cache any reads */
ssize_t rhizome_read_buffered(struct rhizome_read *read, struct rhizome_read_buffer *buffer, unsigned char *data, size_t len)
{
  size_t bytes_copied=0;
  
  while (len>0){
    //DEBUGF(rhizome_store, "len=%zu read->length=%"PRIu64" read->offset=%"PRIu64" buffer->offset=%"PRIu64"", len, read->length, read->offset, buffer->offset);
    // make sure we only attempt to read data that actually exists
    if (read->length != RHIZOME_SIZE_UNSET && read->offset + len > read->length){
      assert(read->offset <= read->length);
      len = read->length - read->offset;
    }

    // if we can supply either the beginning or end of the data from cache, do that first.
    if (read->offset >= buffer->offset) {
      assert(read->offset - buffer->offset <= SIZE_MAX);
      size_t ofs = read->offset - buffer->offset;
      if (ofs <= buffer->len){
	size_t size = len;
	if (size > buffer->len - ofs)
	  size = buffer->len - ofs;
	if (size > 0){
	  // copy into the start of the data buffer
	  bcopy(buffer->data + ofs, data, size);
	  data+=size;
	  len-=size;
	  read->offset+=size;
	  bytes_copied+=size;
	  continue;
	}
      }
    }
    
    if (read->offset + len > buffer->offset) {
      assert(read->offset + len - buffer->offset <= SIZE_MAX);
      size_t ofs = read->offset + len - buffer->offset;
      if (ofs <= buffer->len){
	size_t size = len;
	if (size > ofs)
	  size = ofs;
	if (size>0){
	  // copy into the end of the data buffer
	  bcopy(buffer->data + ofs - size, data + len - size, size);
	  len-=size;
	  bytes_copied+=size;
	  continue;
	}
      }
    }
    
    // ok, so we need to read at least one buffer to fulfill the request.

    // remember the requested read offset so we can put it back
    uint64_t original_ofs = read->offset;
    // round down to the previous block boundary
    uint64_t read_offset = original_ofs & ~(RHIZOME_CRYPT_PAGE_SIZE -1);

    if (read->length != RHIZOME_SIZE_UNSET && original_ofs + len == read->length){
      // if more than one PAGE is being requested, and the end of the requested range lines up with the end of the file
      // we should probably read the last block first.
      // That way, if the reader is scanning a payload backwards,
      // we will end up caching part of the previous block for the next buffered read
      read_offset = (read->length -1) & ~(RHIZOME_CRYPT_PAGE_SIZE -1);
    }

    buffer->len = 0;
    buffer->offset = read->offset = read_offset;
    ssize_t r = rhizome_read(read, buffer->data, sizeof buffer->data);
    read->offset = original_ofs;
    if (r == -1)
      return -1;
    buffer->len = (size_t) r;
  }
  return bytes_copied;
}

void rhizome_read_close(struct rhizome_read *read)
{
  if (read->length == 0)
    // bzero'd & never opened, or already closed
    return;

  if (read->blob_fd != -1) {
    DEBUGF(rhizome_store, "Closing store fd %d", read->blob_fd);
    close(read->blob_fd);
    read->blob_fd = -1;
  }
  
  if (read->verified==-1) {
    // delete payload!
    rhizome_delete_file(&read->id);
  }else if(read->verified==1) {
    // remember when we verified the file
    time_ms_t now = gettime_ms();
    sqlite_exec_void_loglevel(LOG_LEVEL_WARN, 
      "UPDATE FILES SET last_verified = ? WHERE id=?",
      INT64, now, 
      RHIZOME_FILEHASH_T, &read->id,
      END);
  }
  read->length = 0;
  read->offset = 0;
  read->tail = 0;
}

struct cache_entry{
  struct cache_entry *_left;
  struct cache_entry *_right;
  rhizome_bid_t bundle_id;
  uint64_t version;
  struct rhizome_read read_state;
  time_ms_t expires;
};
struct cache_entry *root;

static struct cache_entry ** find_entry_location(struct cache_entry **ptr, const rhizome_bid_t *bundle_id, uint64_t version)
{
  while(*ptr){
    struct cache_entry *entry = *ptr;
    int cmp = cmp_rhizome_bid_t(bundle_id, &entry->bundle_id);
    if (cmp==0){
      if (entry->version==version)
	break;
      if (version < entry->version)
	ptr = &entry->_left;
      else
	ptr = &entry->_right;
      continue;
    }
    if (cmp<0)
      ptr = &entry->_left;
    else
      ptr = &entry->_right;
  }
  return ptr;
}

static time_ms_t close_entries(struct cache_entry **entry, time_ms_t timeout)
{
  if (!*entry)
    return 0;
    
  time_ms_t ret = close_entries(&(*entry)->_left, timeout);
  time_ms_t t_right = close_entries(&(*entry)->_right, timeout);
  if (t_right!=0 && (t_right < ret || ret==0))
    ret=t_right;
    
  if ((*entry)->expires < timeout || timeout==0){
    rhizome_read_close(&(*entry)->read_state);
    // remember the two children
    struct cache_entry *left=(*entry)->_left;
    struct cache_entry *right=(*entry)->_right;
    // free this entry
    free(*entry);
    // re-add both children to the tree
    *entry=left;
    if (right){
      entry = find_entry_location(entry, &right->bundle_id, right->version);
      *entry=right;
    }
  }else{
    if ((*entry)->expires < ret || ret==0)
      ret=(*entry)->expires;
  }
  return ret;
}

// close any expired cache entries
static void rhizome_cache_alarm(struct sched_ent *alarm)
{
  alarm->alarm = close_entries(&root, gettime_ms());
  if (alarm->alarm){
    alarm->deadline = alarm->alarm + 1000;
    schedule(alarm);
  }
}

static struct profile_total cache_alarm_stats={
  .name="rhizome_cache_alarm",
};
static struct sched_ent cache_alarm={
  .function = rhizome_cache_alarm,
  .stats = &cache_alarm_stats,
};

// close all cache entries
int rhizome_cache_close()
{
  close_entries(&root, 0);
  unschedule(&cache_alarm);
  return 0;
}

static int _rhizome_cache_count(struct cache_entry *entry)
{
  if (!entry)
    return 0;
  return 1+_rhizome_cache_count(entry->_left)+_rhizome_cache_count(entry->_right);
}

int rhizome_cache_count()
{
  return _rhizome_cache_count(root);
}

// read a block of data, caching meta data for reuse
ssize_t rhizome_read_cached(const rhizome_bid_t *bidp, uint64_t version, time_ms_t timeout, uint64_t fileOffset, unsigned char *buffer, size_t length)
{
  // look for a cached entry
  struct cache_entry **ptr = find_entry_location(&root, bidp, version);
  struct cache_entry *entry = *ptr;
  
  // if we don't have one yet, create one and open it
  if (!entry){
    rhizome_filehash_t filehash;
    if (rhizome_database_filehash_from_id(bidp, version, &filehash) != 0){
      DEBUGF(rhizome_store, "Payload not found for bundle bid=%s version=%"PRIu64, 
	     alloca_tohex_rhizome_bid_t(*bidp), version);
      return -1;
    }
    entry = emalloc_zero(sizeof(struct cache_entry));
    if (entry == NULL)
      return -1;
    enum rhizome_payload_status status = rhizome_open_read(&entry->read_state, &filehash);
    switch (status) {
      case RHIZOME_PAYLOAD_STATUS_EMPTY:
      case RHIZOME_PAYLOAD_STATUS_STORED:
	break;
      case RHIZOME_PAYLOAD_STATUS_NEW:
	free(entry);
	return WHYF("Payload %s not found", alloca_tohex_rhizome_filehash_t(filehash));
      case RHIZOME_PAYLOAD_STATUS_ERROR:
      case RHIZOME_PAYLOAD_STATUS_WRONG_SIZE:
      case RHIZOME_PAYLOAD_STATUS_WRONG_HASH:
      case RHIZOME_PAYLOAD_STATUS_CRYPTO_FAIL:
	free(entry);
	return WHYF("Error opening payload %s", alloca_tohex_rhizome_filehash_t(filehash));
      default:
	FATALF("status = %d", status);
    }
    entry->bundle_id = *bidp;
    entry->version = version;
    *ptr = entry;
  }
  
  entry->read_state.offset = fileOffset;
  if (entry->read_state.length != RHIZOME_SIZE_UNSET && fileOffset >= entry->read_state.length)
    return 0;
  
  if (entry->expires < timeout){
    entry->expires = timeout;
    
    if (!cache_alarm.alarm){
      cache_alarm.alarm = timeout;
      cache_alarm.deadline = timeout + 1000;
      schedule(&cache_alarm);
    }
  }
  
  return rhizome_read(&entry->read_state, buffer, length);
}

/* Returns -1 on error, 0 on success.
 */
static int write_file(struct rhizome_read *read, const char *filepath){
  int fd=-1, ret=0;
  
  if (filepath&&filepath[0]) {
    fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0775);
    if (fd == -1)
      return WHY_perror("open");
  }
  
  unsigned char buffer[RHIZOME_CRYPT_PAGE_SIZE];
  while((ret=rhizome_read(read, buffer, sizeof(buffer)))>0){
    if (fd!=-1){
      if (write(fd,buffer,ret)!=ret) {
	ret = WHY_perror("Failed to write data to file");
	break;
      }
    }
  }
  
  if (fd!=-1){
    if (close(fd)==-1)
      ret=WHY_perror("close");
    if (ret<0){
      // TODO delete partial file
    }
  }
  
  return ret;
}

static enum rhizome_payload_status read_derive_key(rhizome_manifest *m, struct rhizome_read *read_state)
{
  read_state->crypt = m->payloadEncryption == PAYLOAD_ENCRYPTED;
  if (read_state->crypt){
    // if the manifest specifies encryption, make sure we can generate the payload key and encrypt
    // the contents as we go
    if (!rhizome_derive_payload_key(m)) {
      rhizome_read_close(read_state);
      WHY("Unable to decrypt bundle, valid key not found");
      return RHIZOME_PAYLOAD_STATUS_CRYPTO_FAIL;
    }
    DEBUGF(rhizome_store, "Decrypting payload contents for bid=%s version=%"PRIu64, alloca_tohex_rhizome_bid_t(m->keypair.public_key), m->version);
    if (m->is_journal && m->tail > 0)
      read_state->tail = m->tail;
    bcopy(m->payloadKey, read_state->key, sizeof(read_state->key));
    bcopy(m->payloadNonce, read_state->nonce, sizeof(read_state->nonce));
  }
  return RHIZOME_PAYLOAD_STATUS_STORED;
}

enum rhizome_payload_status rhizome_open_decrypt_read(rhizome_manifest *m, struct rhizome_read *read_state)
{
  if (m->filesize == 0 && !m->has_filehash)
    return RHIZOME_PAYLOAD_STATUS_EMPTY;
  enum rhizome_payload_status status = rhizome_open_read(read_state, &m->filehash);
  if (status == RHIZOME_PAYLOAD_STATUS_STORED)
    status = read_derive_key(m, read_state);
  return status;
}

/* Extract the file related to a manifest to the file system.  The file will be de-crypted and
 * verified while reading.  If filepath is not supplied, the file will still be checked.
 */
enum rhizome_payload_status rhizome_extract_file(rhizome_manifest *m, const char *filepath)
{
  struct rhizome_read read_state;
  bzero(&read_state, sizeof read_state);
  enum rhizome_payload_status status = rhizome_open_decrypt_read(m, &read_state);
  if (status == RHIZOME_PAYLOAD_STATUS_STORED) {
    if (write_file(&read_state, filepath) == -1)
      status = RHIZOME_PAYLOAD_STATUS_ERROR;
  }
  rhizome_read_close(&read_state);
  return status;
}

/* dump the raw contents of a file
 */
enum rhizome_payload_status rhizome_dump_file(const rhizome_filehash_t *hashp, const char *filepath, uint64_t *lengthp)
{
  struct rhizome_read read_state;
  bzero(&read_state, sizeof read_state);
  enum rhizome_payload_status status = rhizome_open_read(&read_state, hashp);
  if (status == RHIZOME_PAYLOAD_STATUS_STORED) {
    if (write_file(&read_state, filepath) == -1)
      status = RHIZOME_PAYLOAD_STATUS_ERROR;
    else if (lengthp)
      *lengthp = read_state.length;
  }
  rhizome_read_close(&read_state);
  return status;
}

// pipe data from one payload to another
static int rhizome_pipe(struct rhizome_read *read, struct rhizome_write *write, uint64_t length)
{
  assert(write->file_offset <= write->file_length);
  if (length > (uint64_t)(write->file_length - write->file_offset))
    return WHY("Unable to pipe that much data");

  unsigned char buffer[RHIZOME_CRYPT_PAGE_SIZE];
  while(length>0){
    size_t size=sizeof(buffer);
    if (size > length)
      size=length;

    ssize_t r = rhizome_read(read, buffer, size);
    if (r == -1)
      return r;

    length -= (size_t) r;
    
    if (rhizome_write_buffer(write, buffer, (size_t) r))
      return -1;
  }

  return 0;
}

uint64_t rhizome_copy_file_to_blob(int fd, uint64_t id, size_t size)
{
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;

  // use an explicit transaction so we can delay I/O failures until COMMIT so they can be retried.
  if (sqlite_exec_void_retry(&retry, "BEGIN TRANSACTION;", END) == -1)
    return 0;

  sqlite3_blob *blob = NULL;

  uint64_t rowid = rhizome_create_fileblob(&retry, id, size);
  if (rowid == 0)
    goto fail;

  if (sqlite_blob_open_retry(&retry, "main", "FILEBLOBS", "data", rowid, 1 /* read/write */, &blob) == -1)
    goto fail;

  uint8_t buf[16384];
  size_t offset = 0;
  while (offset < size) {
    size_t toread = size - offset;
    if (toread > sizeof buf)
      toread = sizeof buf;
    ssize_t nread = read(fd, buf, toread);
    if (nread == -1) {
      WHYF_perror("read(%d,%p,%zu)", fd, buf, toread);
      goto fail;
    }
    if ((size_t)nread == 0) {
      WHYF("read(%d,%p,%zu) returned 0", fd, buf, toread);
      goto fail;
    }
    if (sqlite_blob_write_retry(&retry, blob, buf, (int)nread, (int)offset) == -1)
      goto fail;
    assert((size_t)nread <= toread);
    offset += (size_t)nread;
  }
  assert(offset == size);
  sqlite_blob_close(blob);
  blob = NULL;

  if (sqlite_exec_void_retry(&retry, "COMMIT;", END) == -1)
    goto fail;

  return rowid;

fail:
  if (blob)
    sqlite_blob_close(blob);
  sqlite_exec_void_retry(&retry, "ROLLBACK;", END);
  return 0;
}

static int append_existing_journal_file(struct rhizome_write *write, const rhizome_filehash_t *hashp, uint64_t length){
  // Try to append directly into the previous journal file, linking them together
  DEBUGF(rhizome, "Attempting to append into journal blob");
  // First, we need to read a previous partial hash state
  char *filehash = alloca_tohex_rhizome_filehash_t(*hashp);
  char existing_path[1024];
  if (!FORMF_RHIZOME_STORE_PATH(existing_path, "%s/%s", RHIZOME_BLOB_SUBDIR, filehash))
    return WHYF("existing path too long?");

  int payloadfd = open(existing_path, O_RDWR, 0664);
  if (payloadfd<0){
    if (errno != ENOENT)
      WHYF_perror("Failed to open existing journal payload %s", existing_path);
    else
      DEBUGF(rhizome, "No existing journal payloadpartial hash state");
    return -1;
  }

  off64_t pos = lseek64(payloadfd, 0, SEEK_END);
  if ((uint64_t)pos != length){
    DEBUGF(rhizome, "Existing journal file is not the right length");
    close(payloadfd);
    return -1;
  }

  char hash_path[1024];
  if (!FORMF_RHIZOME_STORE_PATH(hash_path, "%s/%s", RHIZOME_HASH_SUBDIR, filehash)){
    close(payloadfd);
    return WHYF("hash path too long?");
  }

  int hashfd = open(hash_path, O_RDONLY);
  if (hashfd < 0){
    if (errno != ENOENT)
      WHYF_perror("Failed to open partial hash state %s", hash_path);
    else
      DEBUGF(rhizome, "No partial hash state");
    close(payloadfd);
    return -1;
  }

  struct crypto_hash_sha512_state hash_state;
  ssize_t r = read(hashfd, &hash_state, sizeof hash_state);
  close(hashfd);

  if (r != sizeof hash_state){
    close(payloadfd);
    return WHYF("Expected %u bytes", (unsigned)sizeof hash_state);
  }

  char new_path[1024];
  if (!FORMF_RHIZOME_STORE_PATH(new_path, "%s/%"PRIu64, RHIZOME_BLOB_SUBDIR, write->temp_id)){
    close(payloadfd);
    return WHYF("Temp path too long?");
  }

  if (link(existing_path, new_path)==-1){
    close(payloadfd);
    return WHYF_perror("Failed to link journal payloads together");
  }

  // (write_data always seeks so we don't have to)
  write->written_offset = write->file_offset = length;
  write->blob_fd = payloadfd;
  bcopy(&hash_state, &write->sha512_context, sizeof hash_state);

  // Used by tests
  DEBUGF(rhizome,"Reusing journal payload file, keeping %"PRIu64" existing bytes", length);
  return 1;
}

enum rhizome_payload_status rhizome_journal_pipe(struct rhizome_write *write, const rhizome_filehash_t *hashp, uint64_t start_offset, uint64_t length)
{
  if (length==0)
    return RHIZOME_PAYLOAD_STATUS_EMPTY;

  struct rhizome_read read_state;
  bzero(&read_state, sizeof read_state);
  assert(!write->crypt);

  DEBUGF(rhizome, "Piping journal from %"PRIu64", len %"PRIu64" to %"PRIu64,
    start_offset, length, write->file_offset);

  if (start_offset == 0 && write->file_offset == 0
    && append_existing_journal_file(write, hashp, length)!=-1){
    return RHIZOME_PAYLOAD_STATUS_STORED;
  }

  enum rhizome_payload_status status = rhizome_open_read(&read_state, hashp);
  if (status == RHIZOME_PAYLOAD_STATUS_NEW || status == RHIZOME_PAYLOAD_STATUS_EMPTY)
    status = RHIZOME_PAYLOAD_STATUS_ERROR;
  if (status == RHIZOME_PAYLOAD_STATUS_STORED) {
    read_state.offset = start_offset;
    if (rhizome_pipe(&read_state, write, length) == -1)
      status = RHIZOME_PAYLOAD_STATUS_ERROR;
  }
  rhizome_read_close(&read_state);
  return status;
}

// open an existing journal bundle, advance the head pointer, duplicate the existing content and get ready to add more.
enum rhizome_payload_status rhizome_write_open_journal(struct rhizome_write *write, rhizome_manifest *m, uint64_t advance_by, uint64_t append_size)
{
  assert(m->is_journal);
  assert(m->filesize != RHIZOME_SIZE_UNSET);
  assert(advance_by <= m->filesize);
  uint64_t copy_length = m->filesize - advance_by;
  uint64_t new_filesize = RHIZOME_SIZE_UNSET;
  if (append_size != RHIZOME_SIZE_UNSET) {
    assert(m->filesize + append_size > m->filesize); // no wraparound
    new_filesize = m->filesize + append_size - advance_by;
  }
  if (advance_by > 0)
    rhizome_manifest_set_tail(m, m->tail + advance_by);

  enum rhizome_payload_status status = rhizome_open_write(write, NULL, new_filesize);
  DEBUGF(rhizome, "rhizome_open_write() returned %d %s", status, rhizome_payload_status_message(status));
  if (status == RHIZOME_PAYLOAD_STATUS_NEW) {
    write->journal=1;

    if (copy_length > 0){
      // we don't need to bother decrypting the existing journal payload
      enum rhizome_payload_status rstatus = rhizome_journal_pipe(write, &m->filehash, advance_by, copy_length);
      DEBUGF(rhizome, "rhizome_journal_pipe() returned %d %s", rstatus, rhizome_payload_status_message(rstatus));
      int rstatus_valid = 0;
      switch (rstatus) {
      case RHIZOME_PAYLOAD_STATUS_EMPTY:
      case RHIZOME_PAYLOAD_STATUS_NEW:
      case RHIZOME_PAYLOAD_STATUS_STORED:
	rstatus_valid = 1;
	break;
      case RHIZOME_PAYLOAD_STATUS_BUSY:
      case RHIZOME_PAYLOAD_STATUS_ERROR:
      case RHIZOME_PAYLOAD_STATUS_TOO_BIG:
	rstatus_valid = 1;
	status = rstatus;
	break;
      case RHIZOME_PAYLOAD_STATUS_WRONG_SIZE:
      case RHIZOME_PAYLOAD_STATUS_WRONG_HASH:
      case RHIZOME_PAYLOAD_STATUS_CRYPTO_FAIL:
      case RHIZOME_PAYLOAD_STATUS_EVICTED:
	// rhizome_journal_pipe() should not return any of these codes
	FATALF("rhizome_journal_pipe() returned %d %s", rstatus, rhizome_payload_status_message(rstatus));
      }
      if (!rstatus_valid)
	FATALF("rstatus = %d", rstatus);
    }
  }
  if (status == RHIZOME_PAYLOAD_STATUS_NEW) {
    assert(write->file_offset == copy_length);
    status = rhizome_write_derive_key(m, write);
    DEBUGF(rhizome, "rhizome_write_derive_key() returned %d %s", status, rhizome_payload_status_message(status));
  }
  if (status != RHIZOME_PAYLOAD_STATUS_NEW) {
    rhizome_fail_write(write);
  }
  return status;
}

// Call to finish any payload store operation
enum rhizome_payload_status rhizome_finish_store(struct rhizome_write *write, rhizome_manifest *m, enum rhizome_payload_status status)
{
  DEBUGF(rhizome, "write=%p m=manifest %p, status=%d %s", write, m, status, rhizome_payload_status_message_nonnull(status));
  int status_valid = 0;
  switch (status) {
  case RHIZOME_PAYLOAD_STATUS_EMPTY:
    status_valid = 1;
    assert(write->file_length == 0);
    break;
  case RHIZOME_PAYLOAD_STATUS_NEW:
    assert(write->file_length != RHIZOME_SIZE_UNSET);
    status_valid = 1;
    break;
  case RHIZOME_PAYLOAD_STATUS_STORED:
    assert(write->file_length != RHIZOME_SIZE_UNSET);
    status_valid = 1;
    // TODO: check that stored hash matches received payload's hash
    break;
  case RHIZOME_PAYLOAD_STATUS_WRONG_SIZE:
  case RHIZOME_PAYLOAD_STATUS_WRONG_HASH:
  case RHIZOME_PAYLOAD_STATUS_TOO_BIG:
  case RHIZOME_PAYLOAD_STATUS_CRYPTO_FAIL:
  case RHIZOME_PAYLOAD_STATUS_EVICTED:
  case RHIZOME_PAYLOAD_STATUS_ERROR:
  case RHIZOME_PAYLOAD_STATUS_BUSY:
    status_valid = 1;
    rhizome_fail_write(write);
    return status;
  }
  if (!status_valid)
    FATALF("status = %d", status);
  // Fill in missing manifest fields and check consistency with existing fields.
  if (m->is_journal || m->filesize == RHIZOME_SIZE_UNSET)
    rhizome_manifest_set_filesize(m, write->file_length);
  else if (m->filesize != write->file_length) {
    DEBUGF(rhizome, "m->filesize=%"PRIu64", write->file_length=%"PRIu64, m->filesize, write->file_length);
    return RHIZOME_PAYLOAD_STATUS_WRONG_SIZE;
  }
  if (m->is_journal) {
    // TODO ensure new version is greater than previous version
    rhizome_manifest_set_version(m, m->tail + m->filesize);
  }
  if (m->filesize) {
    if (m->is_journal || !m->has_filehash)
      rhizome_manifest_set_filehash(m, &write->id);
    else if (cmp_rhizome_filehash_t(&write->id, &m->filehash) != 0) {
      DEBUGF(rhizome, "m->filehash=%s, write->id=%s", alloca_tohex_rhizome_filehash_t(m->filehash), alloca_tohex_rhizome_filehash_t(write->id));
      return RHIZOME_PAYLOAD_STATUS_WRONG_HASH;
    }
  } else if (m->is_journal)
    rhizome_manifest_del_filehash(m);
  else if (m->has_filehash)
    return RHIZOME_PAYLOAD_STATUS_WRONG_HASH;
  return status;
}

enum rhizome_payload_status rhizome_append_journal_buffer(rhizome_manifest *m, uint64_t advance_by, uint8_t *buffer, size_t len)
{
  struct rhizome_write write;
  bzero(&write, sizeof write);
  assert(advance_by || (buffer && len));
  enum rhizome_payload_status status = rhizome_write_open_journal(&write, m, advance_by, (uint64_t) len);
  if (status != RHIZOME_PAYLOAD_STATUS_NEW)
    return status;
  if (buffer && len && rhizome_write_buffer(&write, buffer, len) == -1)
    status = RHIZOME_PAYLOAD_STATUS_ERROR;
  else
    status = rhizome_finish_write(&write);
  return rhizome_finish_store(&write, m, status);
}

enum rhizome_payload_status rhizome_append_journal_file(rhizome_manifest *m, uint64_t advance_by, const char *filename)
{
  struct stat stat;
  if (lstat(filename,&stat))
    return WHYF_perror("stat(%s)", alloca_str_toprint(filename));
  struct rhizome_write write;
  bzero(&write, sizeof write);
  enum rhizome_payload_status status = rhizome_write_open_journal(&write, m, advance_by, stat.st_size);
  if (status != RHIZOME_PAYLOAD_STATUS_NEW)
    return status;
  if (stat.st_size != 0 && rhizome_write_file(&write, filename, 0, RHIZOME_SIZE_UNSET) == -1)
    status = RHIZOME_PAYLOAD_STATUS_ERROR;
  else
    status = rhizome_finish_write(&write);
  return rhizome_finish_store(&write, m, status);
}
