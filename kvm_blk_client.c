#include "kvm_blk.h"
extern uint32_t debug_flag;

int kvm_blk_client_handle_cmd(void *opaque)
{
    KvmBlkSession *s = opaque;
	struct kvm_blk_request *br;
	uint32_t cmd = s->recv_hdr.cmd;
	int32_t id = s->recv_hdr.id;
	int ret = 0;

    if (debug_flag == 1) {
    	debug_printf("received cmd %d len %d id %d\n", cmd, s->recv_hdr.payload_len,
					s->recv_hdr.id);
    }

	if (cmd == KVM_BLK_CMD_COMMIT_ACK) {
		if (s->ack_cb)
			s->ack_cb(s->ack_cb_opaque);
		return 0;
	}

	QTAILQ_FOREACH(br, &s->request_list, node)
		if (br->id == id)
			break;

	if (!br) {
		fprintf(stderr, "%s can't find record for id = %d\n",
				__func__, id);
		return -1;
	}

    qemu_mutex_lock(&s->mutex);
	QTAILQ_REMOVE(&s->request_list, br, node);
    qemu_mutex_unlock(&s->mutex);

	// handle WRITE
	if (s->recv_hdr.cmd == KVM_BLK_CMD_WRITE) {
        // for quick write
        goto out;

		int i;
		int ret = s->recv_hdr.payload_len;
		assert(br->is_multiple);
		for (i = 0; i < br->num_reqs; ++i)
			br->reqs[i].cb(br->reqs[i].opaque, ret);
		goto out;
	}

	// handle SYNC_READ
	if (br->cb == NULL) {
		// hack for kvm_blk_rw_co
		br->cb = (void *)0xFFFFFFFF;
		goto out;
	}

	// handle READ
	if (s->recv_hdr.payload_len < 0) {
		br->cb(br->opaque, s->recv_hdr.payload_len);
		goto out;
	}
	
	if (s->recv_hdr.payload_len != br->nb_sectors*512) {
		fprintf(stderr, "%s expect %d, get %d\n", __func__,
				br->nb_sectors*512, s->recv_hdr.payload_len);
	}

	kvm_blk_input_to_iov(s, br->piov);
	br->cb(br->opaque, 0);

out:
	g_free(br);
  	return ret;
}

int kvm_blk_rw_co(BlockDriverState *bs, int64_t sector_num, uint8_t *buf,
                      int nb_sectors, bool is_write)
{
	struct kvm_blk_request *br;
	QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base = (void *)buf,
        .iov_len = nb_sectors * BDRV_SECTOR_SIZE,
    };
    qemu_iovec_init_external(&qiov, &iov, 1);

	assert(!is_write);

	br = kvm_blk_aio_readv(bs, sector_num, &qiov, nb_sectors, NULL, NULL);
	while (br->cb == NULL)
		aio_poll(bdrv_get_aio_context(bs), true);
	return 0;
}

struct kvm_blk_request *kvm_blk_aio_readv(BlockDriverState *bs,
                                        int64_t sector_num,
                                        QEMUIOVector *iov,
                                        int nb_sectors,
                                        BlockCompletionFunc *cb,
                                        void *opaque)
{
	KvmBlkSession *s = kvm_blk_session;
	struct kvm_blk_read_control c;
	struct kvm_blk_request *br;

	assert(s->bs = bs);

	br = g_malloc0(sizeof(*br));
	br->sector = sector_num;
	br->nb_sectors = nb_sectors;
	br->cmd = KVM_BLK_CMD_READ;
	br->session = s;
	
	br->piov = iov;
	br->cb = cb;
	br->opaque = opaque;

	c.sector_num = sector_num;
	c.nb_sectors = nb_sectors;

    qemu_mutex_lock(&s->mutex);

	++s->id;
	br->id = s->id;

	QTAILQ_INSERT_TAIL(&s->request_list, br, node);

	s->send_hdr.cmd = KVM_BLK_CMD_READ;
	s->send_hdr.payload_len = sizeof(c);
	s->send_hdr.id = s->id;
	s->send_hdr.num_reqs = 1;

	kvm_blk_output_append(s, &s->send_hdr, sizeof(s->send_hdr));
  	kvm_blk_output_append(s, &c, sizeof(c));
  	kvm_blk_output_flush(s);

    qemu_mutex_unlock(&s->mutex);

    if (debug_flag == 1) {
		debug_printf("sent read cmd: %ld %d %d\n", (long)c.sector_num, c.nb_sectors, s->id);
	}

	return br;
}

int kvm_blk_aio_multiwrite(BlockDriverState *bs,
        BlockRequest *reqs, int num_reqs)
{
	KvmBlkSession *s = kvm_blk_session;
	struct kvm_blk_read_control c;
	struct kvm_blk_request *br;
	int i, total_len;

	assert(s->bs = bs);

	br = g_malloc0(sizeof(*br));
	br->is_multiple = 1;
	br->num_reqs = num_reqs;

	br->reqs = g_malloc(sizeof(BlockRequest) * num_reqs);
	memcpy(br->reqs, reqs, sizeof(BlockRequest) * num_reqs);

	br->cmd = KVM_BLK_CMD_WRITE;
	br->session = s;
    
	total_len = 0;
	total_len += sizeof(c) * num_reqs;
	for (i = 0; i < num_reqs; ++i) {
		total_len += reqs[i].qiov->size;
	}

    qemu_mutex_lock(&s->mutex);

    write_request_id = s->id;
    printf("********** write_request_id %d **********\n", write_request_id);

	br->id = ++s->id;
	QTAILQ_INSERT_TAIL(&s->request_list, br, node);

	s->send_hdr.cmd = KVM_BLK_CMD_WRITE;
	s->send_hdr.payload_len = total_len;
	s->send_hdr.id = s->id;
	s->send_hdr.num_reqs = num_reqs;
	kvm_blk_output_append(s, &s->send_hdr, sizeof(s->send_hdr));

	for (i = 0; i < num_reqs; ++i) {
		c.sector_num = reqs[i].offset;
		c.nb_sectors = reqs[i].qiov->size;

		kvm_blk_output_append(s, &c, sizeof(c));
		kvm_blk_output_append_iov(s, reqs[i].qiov);

        if (debug_flag == 1) {
			debug_printf("wants to write %ld %d %d\n", (long)c.sector_num,
						c.nb_sectors, s->id);
		}
	}
	kvm_blk_output_flush(s);

    qemu_mutex_unlock(&s->mutex);

    // for quick write
    for (i = 0; i < num_reqs; ++i)
		reqs[i].cb(reqs[i].opaque, 0);

	return 0;
}


static void _kvm_blk_send_cmd(KvmBlkSession *s, int cmd)
{
    qemu_mutex_lock(&s->mutex);

    s->send_hdr.cmd = cmd;
	s->send_hdr.payload_len = 0;

	kvm_blk_output_append(s, &s->send_hdr, sizeof(s->send_hdr));
	kvm_blk_output_flush(s);

    qemu_mutex_unlock(&s->mutex);
}

void kvm_blk_epoch_timer(KvmBlkSession *s)
{
    _kvm_blk_send_cmd(s, KVM_BLK_CMD_EPOCH_TIMER);
}

void kvm_blk_epoch_commit(KvmBlkSession *s)
{
    _kvm_blk_send_cmd(s, KVM_BLK_CMD_COMMIT);
}

void kvm_blk_notify_ft(KvmBlkSession *s)
{
    _kvm_blk_send_cmd(s, KVM_BLK_CMD_FT);
}
