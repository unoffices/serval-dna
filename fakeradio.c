/* 
Serval DNA radio serial modem simulator
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

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "os.h"
#include "xprintf.h"

#define PACKET_SIZE 255
int chars_per_ms=1;
long ber=0;

struct radio_state {
  int fd;
  int state;
  const char *name;
  char commandbuffer[128];
  unsigned cb_len;
  unsigned char txbuffer[1280];
  unsigned txb_len;
  unsigned tx_count;
  unsigned wait_count;
  unsigned char rxbuffer[512];
  unsigned rxb_len;
  int64_t last_char_ms;
  int64_t next_rssi_time_ms;
  int rssi_output;
  unsigned char seqnum;
};

#define STATE_ONLINE 0
#define STATE_PLUS 1
#define STATE_PLUSPLUS 2
#define STATE_PLUSPLUSPLUS 3
#define STATE_COMMAND 4

void log_time(){
  struct timeval tv;
  struct tm tm;
  gettimeofday(&tv, NULL);
  localtime_r(&tv.tv_sec, &tm);
  char buf[50];
  if (strftime(buf, sizeof buf, "%T", &tm) == 0)
    fprintf(stderr, "EMPTYTIME___ ");
  else
    fprintf(stderr, "%s.%03u ", buf, (unsigned int)tv.tv_usec / 1000);
}

int append_bytes(struct radio_state *s, const char *bytes, int len)
{
  if (len==-1)
    len = strlen(bytes);
  if (len + s->rxb_len > sizeof(s->rxbuffer))
    return -1;
  bcopy(bytes, &s->rxbuffer[s->rxb_len], len);
  s->rxb_len+=len;
  return len;
}

int processCommand(struct radio_state *s)
{
  if (!s->cb_len) return 0;
  s->commandbuffer[s->cb_len]=0;
  char *cmd=s->commandbuffer;
  
  log_time();
  fprintf(stderr, "Processing command from %s \"%s\"\n", s->name, cmd);
  
  if (!strcasecmp(cmd,"AT")) {
    // Noop
    append_bytes(s, "OK\r", -1);
    return 0;
  }
  if (!strcasecmp(cmd,"ATO")) {
    append_bytes(s, "OK\r", -1);
    s->state=STATE_ONLINE;
    return 0;
  }
  if (!strcasecmp(cmd,"AT&T")) {
    append_bytes(s, "OK\r", -1);
    s->rssi_output=0;
    return 0;
  }
  if (!strcasecmp(cmd,"AT&T=RSSI")) {
    append_bytes(s, "OK\r", -1);
    s->rssi_output=1;
    return 0;
  }
  if (!strcasecmp(cmd,"ATI")) {
    append_bytes(s, "RFD900a SIMULATOR 1.6\rOK\r", -1);
    return 0;
  }
  append_bytes(s, "ERROR\r", -1);
  return 1;
}

static void store_char(struct radio_state *s, unsigned char c)
{
  if(s->txb_len<sizeof(s->txbuffer)){
    s->txbuffer[s->txb_len++]=c;
  }else{
    log_time();
    fprintf(stderr, "*** Dropped char %02x\n", c);
  }
}

int read_bytes(struct radio_state *s)
{
  unsigned char buff[8];
  int i;
  int bytes=read(s->fd,buff,sizeof(buff));
  if (bytes<=0)
    return bytes;
  log_time();
  fprintf(stderr, "Read from %s\n", s->name);
  xhexdump(XPRINTF_STDIO(stderr), buff, bytes, "");
  s->last_char_ms = gettime_ms();
  
  // process incoming bytes
  for (i=0;i<bytes;i++){
    
    // either append to a command buffer
    if (s->state==STATE_COMMAND){
      if (buff[i]=='\r'){
	// and process the commend on EOL
	processCommand(s);
	s->cb_len=0;
      
      // backspace characters
      }else if (buff[i]=='\b'||buff[i]=='\x7f'){
	if (s->cb_len>0)
	  s->cb_len--;
      
      // append to command buffer
      }else if (s->cb_len<127)
	s->commandbuffer[s->cb_len++]=buff[i];
      continue;
    }
    
    // or watch for "+++"
    if (buff[i]=='+'){
      if (s->state < STATE_PLUSPLUSPLUS)
	s->state++;
    }else
      s->state=STATE_ONLINE;
    
    // or append to the transmit buffer if there's room
    store_char(s,buff[i]);
  }
  return bytes;
}

void write_bytes(struct radio_state *s)
{
  ssize_t wrote = s->rxb_len;
  if (wrote>8)
    wrote=8;
  if (s->last_char_ms)
    wrote = write(s->fd, s->rxbuffer, wrote);
  if (wrote != -1){
    log_time();
    fprintf(stderr, "Wrote to %s\n", s->name);
    xhexdump(XPRINTF_STDIO(stderr), s->rxbuffer, (size_t)wrote, "");
    if ((size_t)wrote < s->rxb_len)
      bcopy(&s->rxbuffer[(size_t)wrote], s->rxbuffer, s->rxb_len - (size_t)wrote);
    s->rxb_len -= (size_t)wrote;
  }
}

int transmitter=0;
int64_t next_transmit_time=0;

#define MAVLINK10_STX 254
#define RADIO_SOURCE_SYSTEM '3'
#define RADIO_SOURCE_COMPONENT 'D'
#define MAVLINK_MSG_ID_RADIO 166
#define MAVLINK_HDR 8

int MAVLINK_MESSAGE_CRCS[]={72, 39, 190, 92, 191, 217, 104, 119, 0, 219, 60, 186, 10, 0, 0, 0, 0, 0, 0, 0, 89, 159, 162, 121, 0, 149, 222, 110, 179, 136, 66, 126, 185, 147, 112, 252, 162, 215, 229, 128, 9, 106, 101, 213, 4, 229, 21, 214, 215, 14, 206, 50, 157, 126, 108, 213, 95, 5, 127, 0, 0, 0, 57, 126, 130, 119, 193, 191, 236, 158, 143, 0, 0, 104, 123, 131, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 174, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 155, 0, 0, 0, 0, 0, 0, 0, 0, 0, 143, 29, 208, 188, 118, 242, 19, 97, 233, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 178, 224, 60, 106, 7};

uint16_t mavlink_crc(unsigned char *buf,int length)
{
  uint16_t sum = 0xFFFF;
  uint8_t i, stoplen;
  
  stoplen = length + 6;
  
  // MAVLink 1.0 has an extra CRC seed
  buf[length+6] = MAVLINK_MESSAGE_CRCS[buf[5]];
  stoplen++;
  
  i = 1;
  while (i<stoplen) {
    uint8_t tmp;
    tmp = buf[i] ^ (uint8_t)(sum&0xff);
    tmp ^= (tmp<<4);
    sum = (sum>>8) ^ (tmp<<8) ^ (tmp<<3) ^ (tmp>>4);
    i++;
  }
  
  return sum;
}

int build_heartbeat(struct radio_state *s){
  if (s->rxb_len + MAVLINK_HDR + 9 > sizeof(s->rxbuffer))
    return -1;
  log_time();
  fprintf(stderr,"Building heartbeat for %s\n", s->name);
  unsigned char *b=&s->rxbuffer[s->rxb_len];
  b[0] = MAVLINK10_STX;
  b[1] = 9;
  b[2] = s->seqnum++;
  b[3] = RADIO_SOURCE_SYSTEM;
  b[4] = RADIO_SOURCE_COMPONENT;
  b[5] = MAVLINK_MSG_ID_RADIO;
  b[6] = 0; //rxerrors
  b[7] = 0; //rxerrors
  b[8] = 0; //fixed
  b[9] = 0; //fixed
  b[10] = 43; //average RSSI
  b[11] = 35; //remote average RSSI
  int space = sizeof(s->txbuffer) - s->txb_len;
  b[12] = ((space/8)*100) / (sizeof(s->txbuffer)/8); //txbuf space
  b[13] = 20; //noise
  b[14] = 20; //remote noise
  uint16_t crc = mavlink_crc(b, 9);
  b[15]=crc&0xFF;
  b[16]=(crc>>8)&0xFF;
  s->rxb_len += MAVLINK_HDR+9;
  return 0;
}

void transfer_bytes(struct radio_state *radios)
{
  // if there's data to transmit, copy a radio packet from one device to the other
  int receiver = transmitter^1;
  struct radio_state *r = &radios[receiver];
  struct radio_state *t = &radios[transmitter];
  size_t bytes = t->txb_len;
  
  if (bytes > PACKET_SIZE)
    bytes = PACKET_SIZE;
  
  // try to send some number of whole mavlink frames from our buffer
  {
    size_t p=0, send=0;
    while (p < bytes){
      
      if (t->txbuffer[p]==MAVLINK10_STX){
	// a mavlink header
	
	// we can send everything before this header
	if (p>0)
	  send = p-1;
	
	// wait for more bytes or for the next transmit slot
	// TODO add time limit
	if (p+1 >= bytes)
	  break;
	
	// how big is this mavlink frame?
	size_t size = t->txbuffer[p+1];
	
	// if the size is valid, try to send the whole packet at once
	if (size <= PACKET_SIZE - MAVLINK_HDR){
	  // wait for more bytes or for the next transmit slot
	  // TODO add time limit
	  if (p+size+MAVLINK_HDR > bytes)
	    break;
	  
	  // detect when we are about to transmit a heartbeat frame
	  if (size==9 && t->txbuffer[p+5]==0){
	    // reply to the host with a heartbeat
	    build_heartbeat(t);
	  }
	  p+=size+MAVLINK_HDR;
	  send=p;
	  continue;
	}
      }
      
      // no valid mavlink frames? just send as much as we can
      send=p;
      p++;
    }
    
    if (send<bytes && !send){
      if (bytes < PACKET_SIZE && t->wait_count++ <5){
	log_time();
	fprintf(stderr,"Waiting for more bytes for %s\n", t->name);
	xhexdump(XPRINTF_STDIO(stderr), t->txbuffer, bytes, "");
      }else
	send = bytes;
    }
    
    if (send)
      t->wait_count=0;
    bytes=send;
  }
  
  if (bytes>0){
    log_time();
    fprintf(stderr, "Transferring %zd byte packet from %s to %s\n", bytes, t->name, r->name);
  }
  
  unsigned i, j;
  int dropped=0;
  
// preamble length in bits that must arrive intact
#define PREAMBLE_LENGTH (20+8)

  // simulate the probability of a bit error in the packet pre-amble and drop the whole packet
  for (i=0;i<PREAMBLE_LENGTH;i++){
    if (random()<ber)
      dropped=1;
  }
  
  if (dropped){
    fprintf(stderr,"Dropped the whole radio packet due to bit flip in the pre-amble\n");
  }else{
    for (i=0;i<bytes && r->rxb_len<sizeof(r->rxbuffer);i++){
      char byte = t->txbuffer[i];
      // introduce bit errors
      for(j=0;j<8;j++) {
	if (random()<ber) {
	  byte^=(1<<j);
	  fprintf(stderr,"Flipped a bit\n");
	}
      }
      r->rxbuffer[r->rxb_len++]=byte;
    }
  }
  
  if (bytes>0 && bytes < t->txb_len)
    bcopy(&t->txbuffer[bytes], t->txbuffer, t->txb_len - bytes);
  t->txb_len-=bytes;
  
  // set the wait time for the next transmission
  next_transmit_time = gettime_ms() + 5 + bytes/chars_per_ms;
  
  if (bytes==0 || t->tx_count == 0 || --t->tx_count == 0){
    // swap who's turn it is to transmit after sending 3 packets or running out of data.
    transmitter = receiver;
    r->tx_count=3;
    // add Tx->Rx change time (it's about 40ms between receiving empty packets)
    next_transmit_time+=15;
  }
}

int calc_ber(double target_packet_fraction)
{
  int byte_count=220+32;
  int max_error_bytes=16;

  int ber;
  int p;
  int byte;
  int bit;

  // 9,000,000 gives a packet delivery rate of ~99%
  // so no point starting smaller than that.
  // Only ~30,000,000 reduces packet delivery rate to
  // ~1%, so the search range is fairly narrow.
  ber=0;
  if (target_packet_fraction<=0.9) ber=6900000;
  if (target_packet_fraction<=0.5) ber=16900000;
  if (target_packet_fraction<=0.25) ber=20600000;
  if (target_packet_fraction<=0.1) ber=23400000;
  if (target_packet_fraction<=0.05) ber=28600000;

  for(;ber<0x70ffffff;ber+=100000)
    {
      int packet_errors=0;
      for(p=0;p<1000;p++) {
	int byte_errors=0;
	int dropped = 0;
	for (byte=0;byte<PREAMBLE_LENGTH;byte++){
	  if (random()<ber){
	    dropped = 1;
	    break;
	  }
	}
	if (!dropped){
	  for(byte=0;byte<byte_count;byte++) {
	    for(bit=0;bit<8;bit++) if (random()<ber) { byte_errors++; break; }
	    if (byte_errors>max_error_bytes) { dropped=1; break; }
	  }
	}
	if (dropped)
	  packet_errors++;
      }
      if (packet_errors>=((1.0-target_packet_fraction)*1000)) break;
    }
  fprintf(stderr,"ber magic value=%d\n",ber);
  return ber;
}

int main(int argc,char **argv)
{
  if (argc>=1) {
    chars_per_ms=atol(argv[1]);
    if (argc>=2) 
      ber=calc_ber(atof(argv[2]));
  }
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srandom((getpid() << 16) ^ tv.tv_sec ^ tv.tv_usec);
  }
  struct pollfd fds[2];
  struct radio_state radios[2];
  
  bzero(&radios,sizeof radios);
  
  int i;
  radios[0].name="left";
  radios[1].name="right";
  for (i=0;i<2;i++){
    radios[i].fd=posix_openpt(O_RDWR|O_NOCTTY);
    grantpt(radios[i].fd);
    unlockpt(radios[i].fd);
    fcntl(radios[i].fd,F_SETFL,fcntl(radios[i].fd, F_GETFL, NULL)|O_NONBLOCK);
    fprintf(stdout,"%s:%s\n", radios[i].name, ptsname(radios[i].fd));
    fds[i].fd = radios[i].fd;
  }
  fflush(stdout);

  fprintf(stderr, "Sending %d bytes per ms\n", chars_per_ms);
  fprintf(stderr, "Introducing %f%% bit errors\n", (ber * 100.0) / 0xFFFFFFFF);
  
  while(1) {
    // what events do we need to poll for? how long can we block?
    int64_t now = gettime_ms();
    int64_t next_event = now+10000;
    
    for (i=0;i<2;i++){
      // always watch for incoming data, though we will throw it away if we run out of buffer space
      fds[i].events = POLLIN;
      // if we have data to write data, watch for POLLOUT too.
      if (radios[i].rxb_len)
	fds[i].events |= POLLOUT;
      
      if (radios[i].rssi_output && next_event > radios[i].next_rssi_time_ms)
	next_event = radios[i].next_rssi_time_ms;
      
      if (radios[i].state==STATE_PLUSPLUSPLUS && next_event > radios[i].last_char_ms+1000)
	next_event = radios[i].last_char_ms+1000;
      
      if (radios[i].txb_len && next_event > next_transmit_time)
	next_event = next_transmit_time;
    }
    
    int delay = next_event - now;
    if (delay<0)
      delay=0;
    
    poll(fds,2,delay);
    
    for (i=0;i<2;i++){
      
      if (fds[i].revents & POLLIN)
	read_bytes(&radios[i]);
	
      if (fds[i].revents & POLLOUT)
	write_bytes(&radios[i]);
      
      now = gettime_ms();
      if (radios[i].rssi_output && now >= radios[i].next_rssi_time_ms){
	if (append_bytes(&radios[i], "L/R RSSI: 200/190  L/R noise: 80/70 pkts: 10  txe=0 rxe=0 stx=0 srx=0 ecc=0/0 temp=42 dco=0\r\n", -1)>0)
	  radios[i].next_rssi_time_ms=now+1000;
      }
      
      if (radios[i].state==STATE_PLUSPLUSPLUS && now >= radios[i].last_char_ms+1000){
	fprintf(stderr, "Detected +++ from %s\n",radios[i].name);
	if (append_bytes(&radios[i], "OK\r\n", -1)>0)
	  radios[i].state=STATE_COMMAND;
      }
    }
    
    if (now >= next_transmit_time)
      transfer_bytes(radios);
  }
  
  return 0;
}
