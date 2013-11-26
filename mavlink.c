// -*- Mode: C; c-basic-offset: 2; -*-
//
// Copyright (c) 2012 Andrew Tridgell, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

/*
Portions Copyright (C) 2013 Paul Gardner-Stephen
 
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

#include "serval.h"
#include "conf.h"
#include "overlay_buffer.h"
#include "golay.h"

#define MAVLINK_MSG_ID_RADIO 166
#define MAVLINK_MSG_ID_DATASTREAM 67
int MAVLINK_MESSAGE_CRCS[]={72, 39, 190, 92, 191, 217, 104, 119, 0, 219, 60, 186, 10, 0, 0, 0, 0, 0, 0, 0, 89, 159, 162, 121, 0, 149, 222, 110, 179, 136, 66, 126, 185, 147, 112, 252, 162, 215, 229, 128, 9, 106, 101, 213, 4, 229, 21, 214, 215, 14, 206, 50, 157, 126, 108, 213, 95, 5, 127, 0, 0, 0, 57, 126, 130, 119, 193, 191, 236, 158, 143, 0, 0, 104, 123, 131, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 174, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 155, 0, 0, 0, 0, 0, 0, 0, 0, 0, 143, 29, 208, 188, 118, 242, 19, 97, 233, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 178, 224, 60, 106, 7};

// use '3D' for 3DRadio
#define RADIO_SOURCE_SYSTEM '3'
#define RADIO_SOURCE_COMPONENT 'D'

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
  buf[length+6]=sum&0xff;
  buf[length+7]=sum>>8;
  
  return sum;
}


/*
  we use a hand-crafted MAVLink packet based on the following
  message definition
  
  <message name="RADIO" id="166">
  <description>Status generated by radio</description>
  <field type="uint8_t" name="rssi">local signal strength</field>
  <field type="uint8_t" name="remrssi">remote signal strength</field>
  <field type="uint8_t" name="txbuf">percentage free space in transmit buffer</field>
  <field type="uint8_t" name="noise">background noise level</field>
  <field type="uint8_t" name="remnoise">remote background noise level</field>
  <field type="uint16_t" name="rxerrors">receive errors</field>
  <field type="uint16_t" name="fixed">count of error corrected packets</field>
  </message>
*/
struct mavlink_RADIO_v09 {
  uint8_t rssi;
  uint8_t remrssi;
  uint8_t txbuf;
  uint8_t noise;
  uint8_t remnoise;
  uint16_t rxerrors;
  uint16_t fixed;
};
struct mavlink_RADIO_v10 {
  uint16_t rxerrors;
  uint16_t fixed;
  uint8_t rssi;
  uint8_t remrssi;
  uint8_t txbuf;
  uint8_t noise;
  uint8_t remnoise;
};

/*
  Each mavlink frame consists of 0xfe followed by a standard 6 byte header.
  Normally the payload plus a 2-byte CRC follows.
  We are replacing the CRC check with a Reed-Solomon code to correct as well
  as detect upto 16 bytes with errors, in return for a 32-byte overhead.

  The nature of the particular library we are using is that the overhead is
  basically fixed, but we can shorten the data section.  

  Note that the mavlink headers are not protected against errors.  This is a
  limitation of the radio firmware at present. One day we will re-write the
  radio firmware so that we can send and receive raw radio frames, and get
  rid of the mavlink framing altogether, and just send R-S protected payloads.

  Not ideal, but will be fine for now.
*/

#include "fec-3.0.1/fixed.h"
void encode_rs_8(data_t *data, data_t *parity,int pad);
int decode_rs_8(data_t *data, int *eras_pos, int no_eras, int pad);

int mavlink_encode_packet(struct overlay_interface *interface)
{
  int count = ob_remaining(interface->tx_packet);
  int startP = (ob_position(interface->tx_packet) == 0);
  int endP = 1;
  if (count+6+32 > 255){
    count = 255-6-32;
    endP = 0;
  }
  interface->txbuffer[0]=0xfe; // mavlink v1.0 frame
  /* payload len, excluding 6 byte header and 2 byte CRC.
     But we use a 4-byte CRC, so need to add two to count to make packet lengths
     be as expected.
     Note that this construction will result in CRC errors by non-servald
     programmes, which is probably more helpful than otherwise.
  */
  // we need 32 bytes for the parity, but this field assumes
  // that there is a 2 byte CRC, so we can save two bytes
  int len = count+32 - 2;
  interface->txbuffer[1]=len;
  interface->txbuffer[2]=(len & 0xF);
  interface->txbuffer[3]=0;
  golay_encode(&interface->txbuffer[1]);
  
  interface->txbuffer[4]=(interface->mavlink_seq++) & 0x3f;
  if (startP) interface->txbuffer[4]|=0x40;
  if (endP) interface->txbuffer[4]|=0x80;
  interface->txbuffer[5]=MAVLINK_MSG_ID_DATASTREAM;
  
  ob_get_bytes(interface->tx_packet, &interface->txbuffer[6], count);
  
  encode_rs_8(&interface->txbuffer[4], &interface->txbuffer[6+count], 223 - (count+2));
  interface->tx_bytes_pending=len + 8;
  if (endP){
    ob_free(interface->tx_packet);
    interface->tx_packet=NULL;
    overlay_queue_schedule_next(gettime_ms());
  }
  return 0;
}

int mavlink_heartbeat(unsigned char *frame,int *outlen)
{
  int count=9;
  bzero(frame, count+8);
  
  frame[0]=0xfe; // mavlink v1.0 frame
  // Must be 9 to indicate heartbeat
  frame[1]=count; // payload len, excluding 6 byte header and 2 byte CRC
  frame[2]=(count & 0xF); // packet sequence
  frame[3]=0x00; // system ID of sender (MAV_TYPE_GENERIC)
  golay_encode(&frame[1]);
  frame[4]=0xf1; // component ID of sender (MAV_COMP_ID_UART_BRIDGE)
  // Must be zero to indicate heartbeat
  frame[5]=0; // message ID type of this frame: DATA_STREAM

  // extra magic number to detect remote heartbeat requests
  frame[14]=0x55;
  frame[15]=0x05;
  golay_encode(&frame[14]);
  
  *outlen=count+8;
  
  return 0;
}

static int parse_heartbeat(struct overlay_interface *interface, const unsigned char *payload)
{
  if (payload[0]==0xFE 
    && payload[1]==9
    && payload[3]==RADIO_SOURCE_SYSTEM
    && payload[4]==RADIO_SOURCE_COMPONENT
    && payload[5]==MAVLINK_MSG_ID_RADIO){
    
    // we can assume that radio status packets arrive without corruption
    interface->radio_rssi=(1.0*payload[10]-payload[13])/1.9;
    interface->remote_rssi=(1.0*payload[11] - payload[14])/1.9;
    int free_space = payload[12];
    int free_bytes = (free_space * 1280) / 100 - 30;
    interface->remaining_space = free_bytes;
    if (free_bytes>0)
      interface->next_tx_allowed = gettime_ms();
    if (free_bytes>720)
      interface->next_heartbeat=gettime_ms()+1000;
    if (config.debug.packetradio) {
      INFOF("Link budget = %+ddB, remote link budget = %+ddB, buffer space = %d%% (approx %d)",
	    interface->radio_rssi,
	    interface->remote_rssi,
	    free_space, free_bytes);
    }
    return 1;
  }
  return 0;
}

static int mavlink_parse(struct overlay_interface *interface, struct slip_decode_state *state, 
  int packet_length, unsigned char *payload, int *backtrack)
{
  *backtrack=0;
  if (packet_length==9){
    // make sure we've heard the start and end of a remote heartbeat request
    int errs=0;
    int tail = golay_decode(&errs, &payload[14]);
    if (tail == 0x555){
      return 1;
    }
    return 0;
  }
  
  int data_bytes = packet_length - (32 - 2);
  // preserve the last 16 bytes of data
  unsigned char old_footer[32];
  unsigned char *payload_footer=&payload[packet_length+8-sizeof(old_footer)];
  bcopy(payload_footer, old_footer, sizeof(old_footer));
  
  int pad=223 - (data_bytes + 2);
  int errors=decode_rs_8(&payload[4], NULL, 0, pad);
  if (errors==-1){
    if (config.debug.mavlink)
      DEBUGF("Reed-Solomon error correction failed");
    return 0;
  }
  *backtrack=errors;
  
  int seq=payload[4]&0x3f;
  
  if (config.debug.mavlink){
    DEBUGF("Received RS protected message, len: %d, errors: %d, seq: %d, flags:%s%s", 
      data_bytes,
      errors,
      seq,
      payload[4]&0x40?" start":"",
      payload[4]&0x80?" end":"");
  }
  
  if (seq != ((state->mavlink_seq+1)&0x3f)){
    // reject partial packet if we missed a sequence number
    if (config.debug.mavlink) 
      DEBUGF("Rejecting packet, sequence jumped from %d to %d", state->mavlink_seq, seq);
    state->packet_length=sizeof(state->dst)+1;
  }
  
  if (payload[4]&0x40){
    // start a new packet
    state->packet_length=0;
  }
  
  state->mavlink_seq=payload[4]&0x3f;
  if (state->packet_length + data_bytes > sizeof(state->dst)){
    if (config.debug.mavlink)
      DEBUG("Fragmented packet is too long or a previous piece was missed - discarding");
    state->packet_length=sizeof(state->dst)+1;
    return 1;
  }
  
  bcopy(&payload[6], &state->dst[state->packet_length], data_bytes);
  state->packet_length+=data_bytes;
    
  if (payload[4]&0x80) {
    if (config.debug.mavlink) 
      DEBUGF("PDU Complete (length=%d)",state->packet_length);
    state->dst_offset=0;
    
    packetOkOverlay(interface, state->dst, state->packet_length, -1, NULL, 0);
    state->packet_length=sizeof(state->dst)+1;
  }
  return 1;
}

static int decode_length(struct slip_decode_state *state, unsigned char *p)
{
  // look for a valid golay encoded length
  int errs=0;
  int length = golay_decode(&errs, p);
  if (length<0 || ((length >>8) & 0xF) != (length&0xF))
    return -1;
  length=length&0xFF;
  if (length!=9 && (length<31 || length+8>255))
    return -1;
  
  if (config.debug.mavlink && (errs || state->mavlink_payload_length!=*p))
    DEBUGF("Decoded length %d to %d with %d errs", *p, length, errs);
  
  state->mavlink_payload_length=length;
  return 0;
}

int mavlink_decode(struct overlay_interface *interface, struct slip_decode_state *state, uint8_t c)
{
  if (state->mavlink_payload_start + state->mavlink_payload_offset >= sizeof(state->mavlink_payload)){
    // drop one byte if we run out of space
    if (config.debug.mavlink)
      DEBUGF("Dropped %02x, buffer full", state->mavlink_payload[0]);
    bcopy(state->mavlink_payload+1, state->mavlink_payload, sizeof(state->mavlink_payload) -1);
    state->mavlink_payload_start--;
  }
  
  unsigned char *p = &state->mavlink_payload[state->mavlink_payload_start];
  p[state->mavlink_payload_offset++]=c;
  
  while(1){
    // look for packet length headers
    p = &state->mavlink_payload[state->mavlink_payload_start];
    while(state->mavlink_payload_length==0 && state->mavlink_payload_offset>=6){
      if (p[0]==0xFE 
	&& p[1]==9
	&& p[3]==RADIO_SOURCE_SYSTEM
	&& p[4]==RADIO_SOURCE_COMPONENT
	&& p[5]==MAVLINK_MSG_ID_RADIO){
	//looks like a valid heartbeat response header, read the rest and process it
	state->mavlink_payload_length=9;
	break;
      }
      
      if (decode_length(state, &p[1])==0)
	break;
      
      state->mavlink_payload_start++;
      state->mavlink_payload_offset--;
      p++;
    }
    
    // wait for a whole packet
    if (!state->mavlink_payload_length || state->mavlink_payload_offset < state->mavlink_payload_length+8)
      return 0;
    
    if (parse_heartbeat(interface, p)){
      // cut the bytes of the heartbeat out of the buffer
      state->mavlink_payload_offset -= state->mavlink_payload_length+8;
      if (state->mavlink_payload_offset){
	// shuffle bytes backwards
	bcopy(&p[state->mavlink_payload_length+8], p, state->mavlink_payload_offset);
      }
      // restart parsing for a valid header from the beginning of out buffer
      state->mavlink_payload_offset+=state->mavlink_payload_start;
      state->mavlink_payload_start=0;
      state->mavlink_payload_length=0;
      continue;
    }
    
    // is this a well formed packet?
    int backtrack=0;
    if (mavlink_parse(interface, state, state->mavlink_payload_length, p, &backtrack)==1){
      // Since we know we've synced with the remote party, 
      // and there's nothing we can do about any earlier data
      // throw away everything before the end of this packet
      if (state->mavlink_payload_start && config.debug.mavlink)
	dump("Skipped", state->mavlink_payload, state->mavlink_payload_start);
      
      // If the packet is truncated by less than 16 bytes, RS protection should be enough to recover the packet, 
      // but we may need to examine the last few bytes to find the start of the next packet.
      state->mavlink_payload_offset -= state->mavlink_payload_length+8-backtrack;
      if (state->mavlink_payload_offset){
	// shuffle all remaining bytes back to the start of the buffer
	bcopy(&state->mavlink_payload[state->mavlink_payload_start + state->mavlink_payload_length+8-backtrack], 
	  state->mavlink_payload, state->mavlink_payload_offset);
      }
      state->mavlink_payload_start=0;
    }else{
      // ignore the first byte for now and start looking for another packet header
      // we may find a heartbeat in the middle that we need to cut out first
      state->mavlink_payload_start++;
      state->mavlink_payload_offset--;
    }
    state->mavlink_payload_length=0;
  };
}
