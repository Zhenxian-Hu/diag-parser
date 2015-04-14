#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <osmocom/gsm/rsl.h>
#include <osmocom/core/gsmtap.h>
#include <osmocom/core/gsmtap_util.h>
#ifdef USE_PCAP
#include <pcap.h>
#include <assert.h>
#include <sys/time.h>
#endif
#include "output.h"

#ifdef USE_PCAP

/* Pcap file header */
struct srl_pcap_file_header
{
	bpf_u_int32 magic;
	u_short version_major;
	u_short version_minor;
	bpf_int32 thiszone;	/* gmt to local correction */
	bpf_u_int32 sigfigs;	/* accuracy of timestamps */
	bpf_u_int32 snaplen;	/* max length saved portion of each pkt */
	bpf_u_int32 linktype;	/* data link type (LINKTYPE_*) */
} __attribute__((packed));
typedef struct srl_pcap_file_header srl_pcap_file_header_t;

/* Pcap packet header */
struct srl_pcap_pkthdr
{
	struct timeval ts;	/* time stamp */
	bpf_u_int32 caplen;	/* length of portion present  */
	bpf_u_int32 len;	/* length this packet (off wire) */
};
typedef struct srl_pcap_pkthdr srl_pcap_pkthdr_t;

/* Pcap handle */
struct srl_pcap_handle
{
	srl_pcap_file_header_t hdr;
	FILE *fd;
};
typedef struct srl_pcap_handle srl_pcap_handle_t;

static srl_pcap_handle_t *pcap_handle = NULL; 	/* Pcap handle */
char pcap_buff[65535+1]; 	/* Working buffer for crafting the pcap packets */
size_t gsmtap_offset; 		/* Offset where the gsmtap payload begins */
size_t udplen_offset;		/* Offset where the udp length is stored */
size_t iphdrchksum_offset;	/* Offset where the ip header checksum is stored */
size_t iptotlen_offset;		/* Ip header total length offset */

#else

static struct gsmtap_inst *gti = NULL;

#endif



#ifdef USE_PCAP
/* Create a new pcap handle */
void srl_pcap_create_handle(srl_pcap_handle_t **handle)
{
	*handle = (srl_pcap_handle_t*) malloc(sizeof(srl_pcap_handle_t));
}


/* Create a new pcap file */
srl_pcap_dump_open(srl_pcap_handle_t *handle, const char *output_file)
{
	int rc;

	/* Just to be sure, zero out the header structure */
	memset(handle,0,sizeof(srl_pcap_handle_t));

	/* Create a new file */
	handle->fd = fopen(output_file,"w");
	assert(handle->fd);

	/* Fill out header with valid data */
	handle->hdr.magic = 0xA1B2C3D4;
	handle->hdr.version_major = 0x0020;
	handle->hdr.version_minor = 0x0040;
	handle->hdr.thiszone = 0x00000000;
	handle->hdr.sigfigs = 0x00000000;
	handle->hdr.snaplen = 0x0000FFFF;	/* Our packet size never exceeds 64k */
	handle->hdr.linktype = 0x00000001;

	/* Write header to file */
	rc = fwrite(handle,sizeof(srl_pcap_file_header_t),1,handle->fd);
	assert(rc == 1);
	fflush(handle->fd);
}

/* Dump a packet into pcap file */
void srl_pcap_dump(srl_pcap_handle_t *handle, srl_pcap_pkthdr_t *header, char *packet)
{
	int rc;
	char len[4];
	char timestamp[8];

	assert(header->caplen == header->len);
	assert(header->caplen <= handle->hdr.snaplen);

	/* Write header */
	memset(timestamp,0,sizeof(timestamp));
	timestamp[3] = (header->ts.tv_sec >> 24) & 0x0FF;
	timestamp[2] = (header->ts.tv_sec >> 16) & 0x0FF;
	timestamp[1] = (header->ts.tv_sec >> 8) & 0x0FF;
	timestamp[0] = header->ts.tv_sec & 0x0FF;

	rc = fwrite(timestamp,sizeof(timestamp),1,handle->fd);
	assert(rc == 1);

	len[3] = (header->caplen >> 24) & 0x0FF;
	len[2] = (header->caplen >> 16) & 0x0FF;
	len[1] = (header->caplen >> 8) & 0x0FF;
	len[0] = header->caplen & 0x0FF;
	rc = fwrite(len,sizeof(len),1,handle->fd);
	assert(rc == 1);
	rc = fwrite(len,sizeof(len),1,handle->fd);
	assert(rc == 1);

	/* Write payload */
	rc = fwrite(packet,header->caplen,1,handle->fd);
	assert(rc == 1);

	fflush(handle->fd);
}

/* Close pcap file */
void srl_pcap_dump_close(srl_pcap_handle_t *handle)
{
	if(handle->fd)
		fclose(handle->fd);
	handle->fd = NULL;
}


/* IP header checksum calculator */
/* Source: http://www.microhowto.info/howto/calculate_an_internet_protocol_checksum_in_c.html */
uint16_t ip_checksum(void* vdata,size_t length) {
	char* data=(char*)vdata;
	uint32_t acc=0xffff;
	size_t i;
	for (i=0;i+1<length;i+=2)
	{
		uint16_t word;
		memcpy(&word,data+i,2);
		acc+=ntohs(word);

		if (acc>0xffff) 
			acc-=0xffff;
	}

	if (length&1)
	{
		uint16_t word=0;
		memcpy(&word,data+length-1,1);
		acc+=ntohs(word);
		if (acc>0xffff)
			acc-=0xffff;
	}

	return htons(~acc);
}

/* Helper function to write some payload data into the pcap file */
static int trace_push_payload(unsigned char *payload_data, int payload_len, struct timeval *timestamp)
{
	struct srl_pcap_pkthdr pcap_pkthdr;
	int ip_hdr_checksum;

	/* Create pcap header */
	assert(payload_len + gsmtap_offset <= 65535);
	if(timestamp) {
		pcap_pkthdr.ts = *timestamp;
	} else {
		memset(&pcap_pkthdr.ts,0,sizeof(struct timeval));
	}
	pcap_pkthdr.len = payload_len + gsmtap_offset;
	pcap_pkthdr.caplen = payload_len + gsmtap_offset;

	/* Copy payload data into pcap buffer */
	memcpy(&pcap_buff[gsmtap_offset],payload_data,payload_len);

	/* Patch udp length field */
	pcap_buff[udplen_offset] = ((payload_len+8) >> 8) & 0xFF;
	pcap_buff[udplen_offset+1] = (payload_len+8) & 0xFF;

	/* Patch ip total length field */
	pcap_buff[iptotlen_offset] = ((gsmtap_offset+payload_len-14) >> 8) & 0xFF;
	pcap_buff[iptotlen_offset+1] = (gsmtap_offset+payload_len-14) & 0xFF;

	/* Patch ip-header checksum */
	ip_hdr_checksum = ip_checksum(pcap_buff,gsmtap_offset);
	pcap_buff[iphdrchksum_offset] = (ip_hdr_checksum >> 8) & 0xFF;
	pcap_buff[iphdrchksum_offset+1] = ip_hdr_checksum & 0xFF;

	/* Dump to pcap file */
	srl_pcap_dump(pcap_handle, &pcap_pkthdr, pcap_buff);

	return 0;
}

#endif

void net_init(const char *target)
{
#ifdef USE_PCAP
	/* Create pcap file */
	srl_pcap_create_handle(&pcap_handle);
	srl_pcap_dump_open(pcap_handle, target);

	/* Prepare buffer with hand-crafted dummy ethernet+ip+udp header */
	char dummy_eth_hdr[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
				0x00,0x00,0x00,0x00,0x08,0x00,0x45,0x00,
				0x00,0x4b,0xb8,0x20,0x40,0x00,0x40,0x11,
				0x00,0x00,0x7f,0x00,0x00,0x01,0x7f,0x00,
				0x00,0x01,0x7a,0x69,0x12,0x79,0xff,0xff,
				0x00,0x00};

	memcpy(pcap_buff,dummy_eth_hdr,sizeof(dummy_eth_hdr));
	gsmtap_offset = sizeof(dummy_eth_hdr);
	udplen_offset = sizeof(dummy_eth_hdr) - 4;
	iphdrchksum_offset = sizeof(dummy_eth_hdr) - 18;
	iptotlen_offset = 16;
#else
	/* GSMTAP init */
	int rc;
	gti = gsmtap_source_init(target, GSMTAP_UDP_PORT, 0);
	if (!gti) {
		fprintf(stderr, "Cannot initialize GSMTAP\n");
		abort();
	}
	rc = gsmtap_source_add_sink(gti);
	assert(rc >= 0);
#endif

}

void net_destroy()
{
#ifdef USE_PCAP
	/* Close pcap file */
	srl_pcap_dump_close(&pcap_handle);
#else
	if (gti) {
		/* Flush GSMTAP message queue */
		while (osmo_select_main(1));

		// Found no counterpart to gsmtap_source_init that
		// would free resources. Doing that by hand, otherwise
		// we run out of file descriptors...
		close(gti->wq.bfd.fd);
		talloc_free(gti);
	}
#endif
}


void net_send_rlcmac(uint8_t *msg, int len, int ts, uint8_t ul)
{
#ifdef USE_PCAP
	struct msgb *msgb;
	if (pcap_handle)
	{
		msgb = gsmtap_makemsg(ul?ARFCN_UPLINK:0, ts, GSMTAP_CHANNEL_PACCH, 0, 0, 0, 0, msg, len);
		trace_push_payload(msgb->data,msgb->data_len,0);
	}
#else
	if (gti) {
		//gsmtap_send(gti, ul?ARFCN_UPLINK:0, 0, 0xd, 0, 0, 0, 0, msg, len);
		gsmtap_send(gti, ul?ARFCN_UPLINK:0, ts, GSMTAP_CHANNEL_PACCH, 0, 0, 0, 0, msg, len);

	}
#endif
}


void net_send_llc(uint8_t *data, int len, uint8_t ul)
{
	struct msgb *msg;
	struct gsmtap_hdr *gh;
	uint8_t *dst;

#ifdef USE_PCAP
	if (!pcap_handle)
		return;
#else
	if (!gti)
		return;
#endif

	if ((data[0] == 0x43) &&
	    (data[1] == 0xc0) &&
	    (data[2] == 0x01))
		return;

	msg = msgb_alloc(sizeof(*gh) + len, "gsmtap_tx");
	if (!msg)
	        return;

	gh = (struct gsmtap_hdr *) msgb_put(msg, sizeof(*gh));

	gh->version = GSMTAP_VERSION;
	gh->hdr_len = sizeof(*gh)/4;
	gh->type = 8;
	gh->timeslot = 0;
	gh->sub_slot = 0;
	gh->arfcn = ul ? htons(ARFCN_UPLINK) : 0;
	gh->snr_db = 0;
	gh->signal_dbm = 0;
	gh->frame_number = 0;
	gh->sub_type = 0;
	gh->antenna_nr = 0;

        dst = msgb_put(msg, len);
        memcpy(dst, data, len);

#ifdef USE_PCAP
	trace_push_payload(msg->data,msg->data_len,0);
#else
	gsmtap_sendmsg(gti, msg);
#endif

}

void net_send_msg(struct radio_message *m)
{
	struct msgb *msgb = 0;
	uint8_t gsmtap_channel;

#ifdef USE_PCAP
	if (!pcap_handle)
		return;
#else
	if (!gti)
		return;
#endif

	if (!(m->flags & MSG_DECODED))
		return;

	switch (m->rat) {
	case RAT_GSM: {
		uint8_t ts, type, subch;

		rsl_dec_chan_nr(m->chan_nr, &type, &subch, &ts);

		gsmtap_channel = chantype_rsl2gsmtap(type, (m->flags & MSG_SACCH) ? 0x40 : 0);

		msgb = gsmtap_makemsg(m->bb.arfcn[0], ts, gsmtap_channel, subch,
				 m->bb.fn[0], m->bb.rxl[0], m->bb.snr[0], m->msg, m->msg_len);
		break;
	}

	case RAT_UMTS:
		if (m->flags & MSG_SDCCH) {
			if (m->bb.arfcn[0] & ARFCN_UPLINK) {
				gsmtap_channel = GSMTAP_RRC_SUB_UL_DCCH_Message;
			} else {
				gsmtap_channel = GSMTAP_RRC_SUB_DL_DCCH_Message;
			}
		} else if (m->flags & MSG_FACCH) {
			if (m->bb.arfcn[0] & ARFCN_UPLINK) {
				gsmtap_channel = GSMTAP_RRC_SUB_UL_CCCH_Message;
			} else {
				gsmtap_channel = GSMTAP_RRC_SUB_DL_CCCH_Message;
			}
		} else if (m->flags & MSG_BCCH) {
			gsmtap_channel = GSMTAP_RRC_SUB_BCCH_BCH_Message;
		} else {
			/* no other types defined */
			return;
		}
		msgb = gsmtap_makemsg_ex(GSMTAP_TYPE_UMTS_RRC, m->bb.arfcn[0], 0,
				 gsmtap_channel, 0, 0, 0, 0, m->bb.data, m->msg_len);
		break;
	case RAT_LTE:
		msgb = gsmtap_makemsg_ex(0x0e, m->bb.arfcn[0], 0,
					 0, 0, 0, 0, 0, m->bb.data, m->msg_len);
		break;
	}

	if (msgb) {
#ifdef USE_PCAP
		int ret = trace_push_payload(msgb->data,msgb->data_len,&m->timestamp);
#else
		int ret = gsmtap_sendmsg(gti, msgb);
#endif
		if (ret != 0) {
			msgb_free(msgb);
		} else {
			osmo_select_main(1);
		}
	}
}

