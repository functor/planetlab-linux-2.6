/* 
 * dvb_demux.c - DVB kernel demux API
 *
 * Copyright (C) 2000-2001 Ralph  Metzler <ralph@convergence.de>
 *		       & Marcus Metzler <marcus@convergence.de>
 *			 for convergence integrated media GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/string.h>
	#include <linux/crc32.h>
#include <asm/uaccess.h>

#include "dvb_demux.h"
#include "dvb_functions.h"

#define NOBUFS  
/* 
** #define DVB_DEMUX_SECTION_LOSS_LOG to monitor payload loss in the syslog
*/
// #define DVB_DEMUX_SECTION_LOSS_LOG


LIST_HEAD(dmx_muxs);


int dmx_register_demux(struct dmx_demux *demux) 
{
	demux->users = 0;
	list_add(&demux->reg_list, &dmx_muxs);
	return 0;
}

int dmx_unregister_demux(struct dmx_demux* demux)
{
	struct list_head *pos, *n, *head=&dmx_muxs;

	list_for_each_safe (pos, n, head) {
		if (DMX_DIR_ENTRY(pos) == demux) {
			if (demux->users>0)
				return -EINVAL;
			list_del(pos);
			return 0;
		}
	}

	return -ENODEV;
}


struct list_head *dmx_get_demuxes(void)
{
	if (list_empty(&dmx_muxs))
		return NULL;

	return &dmx_muxs;
}

/******************************************************************************
 * static inlined helper functions
 ******************************************************************************/


static inline u16 section_length(const u8 *buf)
{
	return 3+((buf[1]&0x0f)<<8)+buf[2];
}


static inline u16 ts_pid(const u8 *buf)
{
	return ((buf[1]&0x1f)<<8)+buf[2];
}


static inline u8 payload(const u8 *tsp)
{
	if (!(tsp[3]&0x10)) // no payload?
		return 0;
	if (tsp[3]&0x20) {  // adaptation field?
		if (tsp[4]>183)    // corrupted data?
			return 0;
		else
			return 184-1-tsp[4];
	}
	return 184;
}


void dvb_set_crc32(u8 *data, int length)
{
	u32 crc;

	crc = crc32_be(~0, data, length);

	data[length]   = (crc >> 24) & 0xff;
	data[length+1] = (crc >> 16) & 0xff;
	data[length+2] = (crc >>  8) & 0xff;
	data[length+3] = (crc)       & 0xff;
}


static u32 dvb_dmx_crc32 (struct dvb_demux_feed *f, const u8 *src, size_t len)
{
	return (f->feed.sec.crc_val = crc32_be (f->feed.sec.crc_val, src, len));
}


static void dvb_dmx_memcopy (struct dvb_demux_feed *f, u8 *d, const u8 *s, size_t len)
{
	memcpy (d, s, len);
}


/******************************************************************************
 * Software filter functions
 ******************************************************************************/

static inline int dvb_dmx_swfilter_payload (struct dvb_demux_feed *feed, const u8 *buf) 
{
	int count = payload(buf);
	int p;
	//int ccok;
	//u8 cc;

	if (count == 0)
		return -1;

	p = 188-count;

	/*
	cc=buf[3]&0x0f;
	ccok=((dvbdmxfeed->cc+1)&0x0f)==cc ? 1 : 0;
	dvbdmxfeed->cc=cc;
	if (!ccok)
		printk("missed packet!\n");
	*/

	if (buf[1] & 0x40)  // PUSI ?
		feed->peslen = 0xfffa;

	feed->peslen += count;

	return feed->cb.ts (&buf[p], count, NULL, 0, &feed->feed.ts, DMX_OK); 
}


static int dvb_dmx_swfilter_sectionfilter (struct dvb_demux_feed *feed, 
				    struct dvb_demux_filter *f)
{
	u8 neq = 0;
	int i;

	for (i=0; i<DVB_DEMUX_MASK_MAX; i++) {
		u8 xor = f->filter.filter_value[i] ^ feed->feed.sec.secbuf[i];

		if (f->maskandmode[i] & xor)
			return 0;

		neq |= f->maskandnotmode[i] & xor;
	}

	if (f->doneq & !neq)
		return 0;

	return feed->cb.sec (feed->feed.sec.secbuf, feed->feed.sec.seclen, 
			     NULL, 0, &f->filter, DMX_OK); 
}


static inline int dvb_dmx_swfilter_section_feed (struct dvb_demux_feed *feed)
{
	struct dvb_demux *demux = feed->demux;
	struct dvb_demux_filter *f = feed->filter;
	struct dmx_section_feed *sec = &feed->feed.sec;
	int section_syntax_indicator;

	if (!sec->is_filtering)
		return 0;

	if (!f)
		return 0;

	if (sec->check_crc) {
		section_syntax_indicator = ((sec->secbuf[1] & 0x80) != 0);
		if (section_syntax_indicator &&
		    demux->check_crc32(feed, sec->secbuf, sec->seclen))
		return -1;
	}

	do {
		if (dvb_dmx_swfilter_sectionfilter(feed, f) < 0)
			return -1;
	} while ((f = f->next) && sec->is_filtering);

	sec->seclen = 0;

	return 0;
}


static void dvb_dmx_swfilter_section_new(struct dvb_demux_feed *feed)
{
	struct dmx_section_feed *sec = &feed->feed.sec;

#ifdef DVB_DEMUX_SECTION_LOSS_LOG
	if(sec->secbufp < sec->tsfeedp)
	{
		int i, n = sec->tsfeedp - sec->secbufp;

		/* section padding is done with 0xff bytes entirely.
		** due to speed reasons, we won't check all of them
		** but just first and last
		*/
		if(sec->secbuf[0] != 0xff || sec->secbuf[n-1] != 0xff)
		{
			printk("dvb_demux.c section ts padding loss: %d/%d\n", 
			       n, sec->tsfeedp);
			printk("dvb_demux.c pad data:");
			for(i = 0; i < n; i++)
				printk(" %02x", sec->secbuf[i]);
			printk("\n");
			}
			}
#endif

	sec->tsfeedp = sec->secbufp = sec->seclen = 0;
	sec->secbuf = sec->secbuf_base;
		}

/* 
** Losless Section Demux 1.4 by Emard
*/
static int dvb_dmx_swfilter_section_copy_dump(struct dvb_demux_feed *feed, const u8 *buf, u8 len)
{
	struct dvb_demux *demux = feed->demux;
	struct dmx_section_feed *sec = &feed->feed.sec;
	u16 limit, seclen, n;

	if(sec->tsfeedp >= DMX_MAX_SECFEED_SIZE)
		return 0;

	if(sec->tsfeedp + len > DMX_MAX_SECFEED_SIZE)
	{
#ifdef DVB_DEMUX_SECTION_LOSS_LOG
		printk("dvb_demux.c section buffer full loss: %d/%d\n", 
		       sec->tsfeedp + len - DMX_MAX_SECFEED_SIZE, DMX_MAX_SECFEED_SIZE);
#endif
		len = DMX_MAX_SECFEED_SIZE - sec->tsfeedp;
	}

	if(len <= 0)
		return 0;

	demux->memcopy(feed, sec->secbuf_base + sec->tsfeedp, buf, len);
	sec->tsfeedp += len;

	/* -----------------------------------------------------
	** Dump all the sections we can find in the data (Emard)
	*/

	limit = sec->tsfeedp;
	if(limit > DMX_MAX_SECFEED_SIZE)
		return -1; /* internal error should never happen */

	/* to be sure always set secbuf */
	sec->secbuf = sec->secbuf_base + sec->secbufp;

	for(n = 0; sec->secbufp + 2 < limit; n++)
	{
		seclen = section_length(sec->secbuf);
		if(seclen <= 0 || seclen > DMX_MAX_SECFEED_SIZE 
		   || seclen + sec->secbufp > limit)
			return 0;
		sec->seclen = seclen;
		sec->crc_val = ~0;
		/* dump [secbuf .. secbuf+seclen) */
		dvb_dmx_swfilter_section_feed(feed);
		sec->secbufp += seclen; /* secbufp and secbuf moving together is */
		sec->secbuf += seclen; /* redundand but saves pointer arithmetic */
		}

		return 0;
	}


static int dvb_dmx_swfilter_section_packet(struct dvb_demux_feed *feed, const u8 *buf) 
{
	u8 p, count;
	int ccok;
	u8 cc;

	count = payload(buf);
		
	if (count == 0)  /* count == 0 if no payload or out of range */
			return -1;

	p = 188-count; /* payload start */

	cc = buf[3] & 0x0f;
	ccok = ((feed->cc+1) & 0x0f) == cc ? 1 : 0;
	feed->cc = cc;
	if(ccok == 0)
	{
#ifdef DVB_DEMUX_SECTION_LOSS_LOG
		printk("dvb_demux.c discontinuity detected %d bytes lost\n", count);
		/* those bytes under sume circumstances will again be reported
		** in the following dvb_dmx_swfilter_section_new
		*/
#endif
		dvb_dmx_swfilter_section_new(feed);
		return 0;
	}

	if(buf[1] & 0x40)
	{
		// PUSI=1 (is set), section boundary is here
		if(count > 1 && buf[p] < count)
		{
			const u8 *before = buf+p+1;
			u8 before_len = buf[p];
			const u8 *after = before+before_len;
			u8 after_len = count-1-before_len;

			dvb_dmx_swfilter_section_copy_dump(feed, before, before_len);
			dvb_dmx_swfilter_section_new(feed);
			dvb_dmx_swfilter_section_copy_dump(feed, after, after_len);
		}
#ifdef DVB_DEMUX_SECTION_LOSS_LOG
		else
			if(count > 0)
				printk("dvb_demux.c PUSI=1 but %d bytes lost\n", count);
#endif
	}
	else
	{
		// PUSI=0 (is not set), no section boundary
		const u8 *entire = buf+p;
		u8 entire_len = count;

		dvb_dmx_swfilter_section_copy_dump(feed, entire, entire_len);
	}
	return 0;
}


static inline void dvb_dmx_swfilter_packet_type(struct dvb_demux_feed *feed, const u8 *buf)
{
	switch(feed->type) {
	case DMX_TYPE_TS:
		if (!feed->feed.ts.is_filtering)
			break;
		if (feed->ts_type & TS_PACKET) {
			if (feed->ts_type & TS_PAYLOAD_ONLY)
				dvb_dmx_swfilter_payload(feed, buf);
			else
				feed->cb.ts(buf, 188, NULL, 0, &feed->feed.ts, DMX_OK); 
		}
		if (feed->ts_type & TS_DECODER) 
			if (feed->demux->write_to_decoder)
				feed->demux->write_to_decoder(feed, buf, 188);
		break;

	case DMX_TYPE_SEC:
		if (!feed->feed.sec.is_filtering)
			break;
		if (dvb_dmx_swfilter_section_packet(feed, buf) < 0)
			feed->feed.sec.seclen = feed->feed.sec.secbufp=0;
		break;

	default:
		break;
	}
}

#define DVR_FEED(f)							\
	(((f)->type == DMX_TYPE_TS) &&					\
	((f)->feed.ts.is_filtering) &&					\
	(((f)->ts_type & (TS_PACKET|TS_PAYLOAD_ONLY)) == TS_PACKET))

void dvb_dmx_swfilter_packet(struct dvb_demux *demux, const u8 *buf)
{
	struct dvb_demux_feed *feed;
	struct list_head *pos, *head=&demux->feed_list;
	u16 pid = ts_pid(buf);
	int dvr_done = 0;

	list_for_each(pos, head) {
		feed = list_entry(pos, struct dvb_demux_feed, list_head);

		if ((feed->pid != pid) && (feed->pid != 0x2000))
			continue;

		/* copy each packet only once to the dvr device, even
		 * if a PID is in multiple filters (e.g. video + PCR) */
		if ((DVR_FEED(feed)) && (dvr_done++))
			continue;

		if (feed->pid == pid) {
			dvb_dmx_swfilter_packet_type (feed, buf);
			if (DVR_FEED(feed))
				continue;
		}

		if (feed->pid == 0x2000)
			feed->cb.ts(buf, 188, NULL, 0, &feed->feed.ts, DMX_OK);
	}
}


void dvb_dmx_swfilter_packets(struct dvb_demux *demux, const u8 *buf, size_t count)
{
	spin_lock(&demux->lock);

	while (count--) {
               	if(buf[0] == 0x47) {
		dvb_dmx_swfilter_packet(demux, buf);
		}
		buf += 188;
	}

	spin_unlock(&demux->lock);
}


void dvb_dmx_swfilter(struct dvb_demux *demux, const u8 *buf, size_t count)
{
	int p = 0,i, j;
	
	spin_lock(&demux->lock);

	if ((i = demux->tsbufp)) {
		if (count < (j=188-i)) {
			memcpy(&demux->tsbuf[i], buf, count);
			demux->tsbufp += count;
			goto bailout;
		}
		memcpy(&demux->tsbuf[i], buf, j);
		if (demux->tsbuf[0] == 0x47)
		dvb_dmx_swfilter_packet(demux, demux->tsbuf);
		demux->tsbufp = 0;
		p += j;
	}

	while (p < count) {
		if (buf[p] == 0x47) {
			if (count-p >= 188) {
				dvb_dmx_swfilter_packet(demux, buf+p);
				p += 188;
			} else {
				i = count-p;
				memcpy(demux->tsbuf, buf+p, i);
				demux->tsbufp=i;
				goto bailout;
			}
		} else 
			p++;
	}

bailout:
	spin_unlock(&demux->lock);
}

void dvb_dmx_swfilter_204(struct dvb_demux *demux, const u8 *buf, size_t count)
{
	int p = 0,i, j;
	u8 tmppack[188];
	spin_lock(&demux->lock);

	if ((i = demux->tsbufp)) {
		if (count < (j=204-i)) {
			memcpy(&demux->tsbuf[i], buf, count);
			demux->tsbufp += count;
			goto bailout;
		}
		memcpy(&demux->tsbuf[i], buf, j);
		if ((demux->tsbuf[0] == 0x47)|(demux->tsbuf[0]==0xB8))  {
			memcpy(tmppack, demux->tsbuf, 188);
			if (tmppack[0] == 0xB8) tmppack[0] = 0x47;
			dvb_dmx_swfilter_packet(demux, tmppack);
		}
		demux->tsbufp = 0;
		p += j;
	}

	while (p < count) {
		if ((buf[p] == 0x47)|(buf[p] == 0xB8)) {
			if (count-p >= 204) {
				memcpy(tmppack, buf+p, 188);
				if (tmppack[0] == 0xB8) tmppack[0] = 0x47;
				dvb_dmx_swfilter_packet(demux, tmppack);
				p += 204;
			} else {
				i = count-p;
				memcpy(demux->tsbuf, buf+p, i);
				demux->tsbufp=i;
				goto bailout;
			}
		} else { 
			p++;
		}
	}

bailout:
	spin_unlock(&demux->lock);
}


static struct dvb_demux_filter * dvb_dmx_filter_alloc(struct dvb_demux *demux)
{
	int i;

	for (i=0; i<demux->filternum; i++)
		if (demux->filter[i].state == DMX_STATE_FREE)
			break;

	if (i == demux->filternum)
		return NULL;

	demux->filter[i].state = DMX_STATE_ALLOCATED;

	return &demux->filter[i];
}

static struct dvb_demux_feed * dvb_dmx_feed_alloc(struct dvb_demux *demux)
{
	int i;

	for (i=0; i<demux->feednum; i++)
		if (demux->feed[i].state == DMX_STATE_FREE)
			break;

	if (i == demux->feednum)
		return NULL;

	demux->feed[i].state = DMX_STATE_ALLOCATED;

	return &demux->feed[i];
}

static int dvb_demux_feed_find(struct dvb_demux_feed *feed)
{
	struct dvb_demux_feed *entry;

	list_for_each_entry(entry, &feed->demux->feed_list, list_head)
		if (entry == feed)
			return 1;

		return 0;
		}

static void dvb_demux_feed_add(struct dvb_demux_feed *feed)
{
	if (dvb_demux_feed_find(feed)) {
		printk(KERN_ERR "%s: feed already in list (type=%x state=%x pid=%x)\n",
				__FUNCTION__, feed->type, feed->state, feed->pid);
		return;
	}

	list_add(&feed->list_head, &feed->demux->feed_list);
}

static void dvb_demux_feed_del(struct dvb_demux_feed *feed)
{
	if (!(dvb_demux_feed_find(feed))) {
		printk(KERN_ERR "%s: feed not in list (type=%x state=%x pid=%x)\n",
				__FUNCTION__, feed->type, feed->state, feed->pid);
		return;
}

	list_del(&feed->list_head);
}

static int dmx_ts_feed_set (struct dmx_ts_feed* ts_feed, u16 pid, int ts_type, 
		     enum dmx_ts_pes pes_type, size_t callback_length, 
		     size_t circular_buffer_size, int descramble, 
		     struct timespec timeout)
{
	struct dvb_demux_feed *feed = (struct dvb_demux_feed *) ts_feed;
	struct dvb_demux *demux = feed->demux;

	if (pid > DMX_MAX_PID)
		return -EINVAL;
	
	if (down_interruptible (&demux->mutex))
		return -ERESTARTSYS;

	if (ts_type & TS_DECODER) {
		if (pes_type >= DMX_TS_PES_OTHER) {
			up(&demux->mutex);
			return -EINVAL;
		}

		if (demux->pesfilter[pes_type] && 
		    demux->pesfilter[pes_type] != feed) {
			up(&demux->mutex);
			return -EINVAL;
		}

		demux->pesfilter[pes_type] = feed;
		demux->pids[pes_type] = pid;
	}

	dvb_demux_feed_add(feed);

	feed->pid = pid;
	feed->buffer_size = circular_buffer_size;
	feed->descramble = descramble;
	feed->timeout = timeout;
	feed->cb_length = callback_length;
	feed->ts_type = ts_type;
	feed->pes_type = pes_type;

	if (feed->descramble) {
		up(&demux->mutex);
		return -ENOSYS;
	}

	if (feed->buffer_size) {
#ifdef NOBUFS
		feed->buffer=NULL;
#else
		feed->buffer = vmalloc(feed->buffer_size);
		if (!feed->buffer) {
			up(&demux->mutex);
			return -ENOMEM;
		}
#endif
	}
	
	feed->state = DMX_STATE_READY;
	up(&demux->mutex);

	return 0;
}


static int dmx_ts_feed_start_filtering(struct dmx_ts_feed* ts_feed)
{
	struct dvb_demux_feed *feed = (struct dvb_demux_feed *) ts_feed;
	struct dvb_demux *demux = feed->demux;
	int ret;

	if (down_interruptible (&demux->mutex))
		return -ERESTARTSYS;

	if (feed->state != DMX_STATE_READY || feed->type != DMX_TYPE_TS) {
		up(&demux->mutex);
		return -EINVAL;
	}

	if (!demux->start_feed) {
		up(&demux->mutex);
		return -ENODEV;
	}

	if ((ret = demux->start_feed(feed)) < 0) {
		up(&demux->mutex);
		return ret;
	}

	spin_lock_irq(&demux->lock);
	ts_feed->is_filtering = 1;
	feed->state = DMX_STATE_GO;
	spin_unlock_irq(&demux->lock);
	up(&demux->mutex);

	return 0;
}
 
static int dmx_ts_feed_stop_filtering(struct dmx_ts_feed* ts_feed)
{
	struct dvb_demux_feed *feed = (struct dvb_demux_feed *) ts_feed;
	struct dvb_demux *demux = feed->demux;
	int ret;

	if (down_interruptible (&demux->mutex))
		return -ERESTARTSYS;

	if (feed->state<DMX_STATE_GO) {
		up(&demux->mutex);
		return -EINVAL;
	}

	if (!demux->stop_feed) {
		up(&demux->mutex);
		return -ENODEV;
	}

	ret = demux->stop_feed(feed); 

	spin_lock_irq(&demux->lock);
	ts_feed->is_filtering = 0;
	feed->state = DMX_STATE_ALLOCATED;
	spin_unlock_irq(&demux->lock);
	up(&demux->mutex);

	return ret;
}

static int dvbdmx_allocate_ts_feed (struct dmx_demux *dmx, struct dmx_ts_feed **ts_feed, 
			     dmx_ts_cb callback)
{
	struct dvb_demux *demux = (struct dvb_demux *) dmx;
	struct dvb_demux_feed *feed;

	if (down_interruptible (&demux->mutex))
		return -ERESTARTSYS;

	if (!(feed = dvb_dmx_feed_alloc(demux))) {
		up(&demux->mutex);
		return -EBUSY;
	}

	feed->type = DMX_TYPE_TS;
	feed->cb.ts = callback;
	feed->demux = demux;
	feed->pid = 0xffff;
	feed->peslen = 0xfffa;
	feed->buffer = NULL;

	(*ts_feed) = &feed->feed.ts;
	(*ts_feed)->parent = dmx;
	(*ts_feed)->priv = NULL;
	(*ts_feed)->is_filtering = 0;
	(*ts_feed)->start_filtering = dmx_ts_feed_start_filtering;
	(*ts_feed)->stop_filtering = dmx_ts_feed_stop_filtering;
	(*ts_feed)->set = dmx_ts_feed_set;


	if (!(feed->filter = dvb_dmx_filter_alloc(demux))) {
		feed->state = DMX_STATE_FREE;
		up(&demux->mutex);
		return -EBUSY;
	}

	feed->filter->type = DMX_TYPE_TS;
	feed->filter->feed = feed;
	feed->filter->state = DMX_STATE_READY;
	
	up(&demux->mutex);

	return 0;
}

static int dvbdmx_release_ts_feed(struct dmx_demux *dmx, struct dmx_ts_feed *ts_feed)
{
	struct dvb_demux *demux = (struct dvb_demux *) dmx;
	struct dvb_demux_feed *feed = (struct dvb_demux_feed *) ts_feed;

	if (down_interruptible (&demux->mutex))
		return -ERESTARTSYS;

	if (feed->state == DMX_STATE_FREE) {
		up(&demux->mutex);
		return -EINVAL;
	}

#ifndef NOBUFS
	if (feed->buffer) { 
		vfree(feed->buffer);
		feed->buffer=0;
	}
#endif

	feed->state = DMX_STATE_FREE;
	feed->filter->state = DMX_STATE_FREE;

	dvb_demux_feed_del(feed);

		feed->pid = 0xffff;
	
	if (feed->ts_type & TS_DECODER)
		demux->pesfilter[feed->pes_type] = NULL;

	up(&demux->mutex);
	return 0;
}


/******************************************************************************
 * dmx_section_feed API calls
 ******************************************************************************/

static int dmx_section_feed_allocate_filter(struct dmx_section_feed* feed, 
				     struct dmx_section_filter** filter) 
{
	struct dvb_demux_feed *dvbdmxfeed=(struct dvb_demux_feed *) feed;
	struct dvb_demux *dvbdemux=dvbdmxfeed->demux;
	struct dvb_demux_filter *dvbdmxfilter;

	if (down_interruptible (&dvbdemux->mutex))
		return -ERESTARTSYS;

	dvbdmxfilter=dvb_dmx_filter_alloc(dvbdemux);
	if (!dvbdmxfilter) {
		up(&dvbdemux->mutex);
		return -EBUSY;
	}

	spin_lock_irq(&dvbdemux->lock);
	*filter=&dvbdmxfilter->filter;
	(*filter)->parent=feed;
	(*filter)->priv=NULL;
	dvbdmxfilter->feed=dvbdmxfeed;
	dvbdmxfilter->type=DMX_TYPE_SEC;
	dvbdmxfilter->state=DMX_STATE_READY;

	dvbdmxfilter->next=dvbdmxfeed->filter;
	dvbdmxfeed->filter=dvbdmxfilter;
	spin_unlock_irq(&dvbdemux->lock);
	up(&dvbdemux->mutex);
	return 0;
}


static int dmx_section_feed_set(struct dmx_section_feed* feed, 
		     u16 pid, size_t circular_buffer_size, 
		     int descramble, int check_crc) 
{
	struct dvb_demux_feed *dvbdmxfeed=(struct dvb_demux_feed *) feed;
	struct dvb_demux *dvbdmx=dvbdmxfeed->demux;

	if (pid>0x1fff)
		return -EINVAL;

	if (down_interruptible (&dvbdmx->mutex))
		return -ERESTARTSYS;
	
	dvb_demux_feed_add(dvbdmxfeed);

	dvbdmxfeed->pid = pid;
	dvbdmxfeed->buffer_size=circular_buffer_size;
	dvbdmxfeed->descramble=descramble;
	if (dvbdmxfeed->descramble) {
		up(&dvbdmx->mutex);
		return -ENOSYS;
	}

	dvbdmxfeed->feed.sec.check_crc=check_crc;
#ifdef NOBUFS
	dvbdmxfeed->buffer=NULL;
#else
	dvbdmxfeed->buffer=vmalloc(dvbdmxfeed->buffer_size);
	if (!dvbdmxfeed->buffer) {
		up(&dvbdmx->mutex);
		return -ENOMEM;
	}
#endif
	dvbdmxfeed->state=DMX_STATE_READY;
	up(&dvbdmx->mutex);
	return 0;
}

static void prepare_secfilters(struct dvb_demux_feed *dvbdmxfeed)
{
	int i;
	struct dvb_demux_filter *f;
	struct dmx_section_filter *sf;
	u8 mask, mode, doneq;
		
	if (!(f=dvbdmxfeed->filter))
		return;
	do {
		sf=&f->filter;
		doneq=0;
		for (i=0; i<DVB_DEMUX_MASK_MAX; i++) {
			mode=sf->filter_mode[i];
			mask=sf->filter_mask[i];
			f->maskandmode[i]=mask&mode;
			doneq|=f->maskandnotmode[i]=mask&~mode;
		}
		f->doneq=doneq ? 1 : 0;
	} while ((f=f->next));
}


static int dmx_section_feed_start_filtering(struct dmx_section_feed *feed)
{
	struct dvb_demux_feed *dvbdmxfeed=(struct dvb_demux_feed *) feed;
	struct dvb_demux *dvbdmx=dvbdmxfeed->demux;
	int ret;

	if (down_interruptible (&dvbdmx->mutex))
		return -ERESTARTSYS;
	
	if (feed->is_filtering) {
		up(&dvbdmx->mutex);
		return -EBUSY;
	}
	if (!dvbdmxfeed->filter) {
		up(&dvbdmx->mutex);
		return -EINVAL;
	}

	dvbdmxfeed->feed.sec.tsfeedp = 0;
	dvbdmxfeed->feed.sec.secbuf = dvbdmxfeed->feed.sec.secbuf_base;
	dvbdmxfeed->feed.sec.secbufp=0;
	dvbdmxfeed->feed.sec.seclen=0;
	
	if (!dvbdmx->start_feed) {
		up(&dvbdmx->mutex);
		return -ENODEV;
	}

	prepare_secfilters(dvbdmxfeed);

	if ((ret = dvbdmx->start_feed(dvbdmxfeed)) < 0) {
		up(&dvbdmx->mutex);
		return ret;
	}

	spin_lock_irq(&dvbdmx->lock);
	feed->is_filtering=1;
	dvbdmxfeed->state=DMX_STATE_GO;
	spin_unlock_irq(&dvbdmx->lock);
	up(&dvbdmx->mutex);
	return 0;
}


static int dmx_section_feed_stop_filtering(struct dmx_section_feed* feed)
{
	struct dvb_demux_feed *dvbdmxfeed=(struct dvb_demux_feed *) feed;
	struct dvb_demux *dvbdmx=dvbdmxfeed->demux;
	int ret;

	if (down_interruptible (&dvbdmx->mutex))
		return -ERESTARTSYS;

	if (!dvbdmx->stop_feed) {
		up(&dvbdmx->mutex);
		return -ENODEV;
	}
	ret=dvbdmx->stop_feed(dvbdmxfeed); 
	spin_lock_irq(&dvbdmx->lock);
	dvbdmxfeed->state=DMX_STATE_READY;
	feed->is_filtering=0;
	spin_unlock_irq(&dvbdmx->lock);
	up(&dvbdmx->mutex);
	return ret;
}


static int dmx_section_feed_release_filter(struct dmx_section_feed *feed, 
				struct dmx_section_filter* filter)
{
	struct dvb_demux_filter *dvbdmxfilter=(struct dvb_demux_filter *) filter, *f;
	struct dvb_demux_feed *dvbdmxfeed=(struct dvb_demux_feed *) feed;
	struct dvb_demux *dvbdmx=dvbdmxfeed->demux;

	if (down_interruptible (&dvbdmx->mutex))
		return -ERESTARTSYS;

	if (dvbdmxfilter->feed!=dvbdmxfeed) {
		up(&dvbdmx->mutex);
		return -EINVAL;
	}
	if (feed->is_filtering) 
		feed->stop_filtering(feed);
	
	spin_lock_irq(&dvbdmx->lock);
	f=dvbdmxfeed->filter;

	if (f == dvbdmxfilter) {
		dvbdmxfeed->filter=dvbdmxfilter->next;
	} else {
		while(f->next!=dvbdmxfilter)
			f=f->next;
		f->next=f->next->next;
	}
	dvbdmxfilter->state=DMX_STATE_FREE;
	spin_unlock_irq(&dvbdmx->lock);
	up(&dvbdmx->mutex);
	return 0;
}

static int dvbdmx_allocate_section_feed(struct dmx_demux *demux, 
					struct dmx_section_feed **feed,
					dmx_section_cb callback)
{
	struct dvb_demux *dvbdmx=(struct dvb_demux *) demux;
	struct dvb_demux_feed *dvbdmxfeed;

	if (down_interruptible (&dvbdmx->mutex))
		return -ERESTARTSYS;

	if (!(dvbdmxfeed=dvb_dmx_feed_alloc(dvbdmx))) {
		up(&dvbdmx->mutex);
		return -EBUSY;
	}
	dvbdmxfeed->type=DMX_TYPE_SEC;
	dvbdmxfeed->cb.sec=callback;
	dvbdmxfeed->demux=dvbdmx;
	dvbdmxfeed->pid=0xffff;
	dvbdmxfeed->feed.sec.secbuf = dvbdmxfeed->feed.sec.secbuf_base;
	dvbdmxfeed->feed.sec.secbufp = dvbdmxfeed->feed.sec.seclen = 0;
	dvbdmxfeed->feed.sec.tsfeedp = 0;
	dvbdmxfeed->filter=NULL;
	dvbdmxfeed->buffer=NULL;

	(*feed)=&dvbdmxfeed->feed.sec;
	(*feed)->is_filtering=0;
	(*feed)->parent=demux;
	(*feed)->priv=NULL;

	(*feed)->set=dmx_section_feed_set;
	(*feed)->allocate_filter=dmx_section_feed_allocate_filter;
	(*feed)->start_filtering=dmx_section_feed_start_filtering;
	(*feed)->stop_filtering=dmx_section_feed_stop_filtering;
	(*feed)->release_filter = dmx_section_feed_release_filter;

	up(&dvbdmx->mutex);
	return 0;
}

static int dvbdmx_release_section_feed(struct dmx_demux *demux, 
				       struct dmx_section_feed *feed)
{
	struct dvb_demux_feed *dvbdmxfeed=(struct dvb_demux_feed *) feed;
	struct dvb_demux *dvbdmx=(struct dvb_demux *) demux;

	if (down_interruptible (&dvbdmx->mutex))
		return -ERESTARTSYS;

	if (dvbdmxfeed->state==DMX_STATE_FREE) {
		up(&dvbdmx->mutex);
		return -EINVAL;
	}
#ifndef NOBUFS
	if (dvbdmxfeed->buffer) {
		vfree(dvbdmxfeed->buffer);
		dvbdmxfeed->buffer=0;
	}
#endif
	dvbdmxfeed->state=DMX_STATE_FREE;

	dvb_demux_feed_del(dvbdmxfeed);

		dvbdmxfeed->pid = 0xffff;

	up(&dvbdmx->mutex);
	return 0;
}


/******************************************************************************
 * dvb_demux kernel data API calls
 ******************************************************************************/

static int dvbdmx_open(struct dmx_demux *demux)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	if (dvbdemux->users>=MAX_DVB_DEMUX_USERS)
		return -EUSERS;
	dvbdemux->users++;
	return 0;
}


static int dvbdmx_close(struct dmx_demux *demux)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	if (dvbdemux->users==0)
		return -ENODEV;
	dvbdemux->users--;
	//FIXME: release any unneeded resources if users==0
	return 0;
}


static int dvbdmx_write(struct dmx_demux *demux, const char *buf, size_t count)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	if ((!demux->frontend) || (demux->frontend->source != DMX_MEMORY_FE))
		return -EINVAL;

	if (down_interruptible (&dvbdemux->mutex))
		return -ERESTARTSYS;
	dvb_dmx_swfilter(dvbdemux, buf, count);
	up(&dvbdemux->mutex);

	if (signal_pending(current))
		return -EINTR;
	return count;
}


static int dvbdmx_add_frontend(struct dmx_demux *demux, struct dmx_frontend *frontend)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;
	struct list_head *head = &dvbdemux->frontend_list;

	list_add(&(frontend->connectivity_list), head);

	return 0;
}


static int dvbdmx_remove_frontend(struct dmx_demux *demux, struct dmx_frontend *frontend)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;
	struct list_head *pos, *n, *head=&dvbdemux->frontend_list;

	list_for_each_safe (pos, n, head) {
		if (DMX_FE_ENTRY(pos) == frontend) {
			list_del(pos);
			return 0;
		}
	}
	return -ENODEV;
}


static struct list_head * dvbdmx_get_frontends(struct dmx_demux *demux)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	if (list_empty(&dvbdemux->frontend_list))
		return NULL;
	return &dvbdemux->frontend_list;
}


int dvbdmx_connect_frontend(struct dmx_demux *demux, struct dmx_frontend *frontend)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	if (demux->frontend)
		return -EINVAL;
	
	if (down_interruptible (&dvbdemux->mutex))
		return -ERESTARTSYS;

	demux->frontend=frontend;
	up(&dvbdemux->mutex);
	return 0;
}


int dvbdmx_disconnect_frontend(struct dmx_demux *demux)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	if (down_interruptible (&dvbdemux->mutex))
		return -ERESTARTSYS;

	demux->frontend=NULL;
	up(&dvbdemux->mutex);
	return 0;
}


static int dvbdmx_get_pes_pids(struct dmx_demux *demux, u16 *pids)
{
	struct dvb_demux *dvbdemux=(struct dvb_demux *) demux;

	memcpy(pids, dvbdemux->pids, 5*sizeof(u16));
	return 0;
}


int dvb_dmx_init(struct dvb_demux *dvbdemux)
{
	int i, err;
	struct dmx_demux *dmx = &dvbdemux->dmx;

	dvbdemux->users=0;
	dvbdemux->filter=vmalloc(dvbdemux->filternum*sizeof(struct dvb_demux_filter));
	if (!dvbdemux->filter)
		return -ENOMEM;

	dvbdemux->feed=vmalloc(dvbdemux->feednum*sizeof(struct dvb_demux_feed));
	if (!dvbdemux->feed) {
		vfree(dvbdemux->filter);
		return -ENOMEM;
	}
	for (i=0; i<dvbdemux->filternum; i++) {
		dvbdemux->filter[i].state=DMX_STATE_FREE;
		dvbdemux->filter[i].index=i;
	}
	for (i=0; i<dvbdemux->feednum; i++)
		dvbdemux->feed[i].state=DMX_STATE_FREE;
	dvbdemux->frontend_list.next=
	  dvbdemux->frontend_list.prev=
	    &dvbdemux->frontend_list;
	for (i=0; i<DMX_TS_PES_OTHER; i++) {
		dvbdemux->pesfilter[i]=NULL;
		dvbdemux->pids[i]=0xffff;
	}

	INIT_LIST_HEAD(&dvbdemux->feed_list);

	dvbdemux->playing = 0;
	dvbdemux->recording = 0;
	dvbdemux->tsbufp=0;

	if (!dvbdemux->check_crc32)
		dvbdemux->check_crc32 = dvb_dmx_crc32;

	 if (!dvbdemux->memcopy)
		 dvbdemux->memcopy = dvb_dmx_memcopy;

	dmx->frontend=NULL;
	dmx->reg_list.prev = dmx->reg_list.next = &dmx->reg_list;
	dmx->priv=(void *) dvbdemux;
	dmx->open=dvbdmx_open;
	dmx->close=dvbdmx_close;
	dmx->write=dvbdmx_write;
	dmx->allocate_ts_feed=dvbdmx_allocate_ts_feed;
	dmx->release_ts_feed=dvbdmx_release_ts_feed;
	dmx->allocate_section_feed=dvbdmx_allocate_section_feed;
	dmx->release_section_feed=dvbdmx_release_section_feed;

	dmx->descramble_mac_address=NULL;
	dmx->descramble_section_payload=NULL;
	
	dmx->add_frontend=dvbdmx_add_frontend;
	dmx->remove_frontend=dvbdmx_remove_frontend;
	dmx->get_frontends=dvbdmx_get_frontends;
	dmx->connect_frontend=dvbdmx_connect_frontend;
	dmx->disconnect_frontend=dvbdmx_disconnect_frontend;
	dmx->get_pes_pids=dvbdmx_get_pes_pids;
	sema_init(&dvbdemux->mutex, 1);
	spin_lock_init(&dvbdemux->lock);

	if ((err = dmx_register_demux(dmx)) < 0) 
		return err;

	return 0;
}


int dvb_dmx_release(struct dvb_demux *dvbdemux)
{
	struct dmx_demux *dmx = &dvbdemux->dmx;

	dmx_unregister_demux(dmx);
	if (dvbdemux->filter)
		vfree(dvbdemux->filter);
	if (dvbdemux->feed)
		vfree(dvbdemux->feed);
	return 0;
}
