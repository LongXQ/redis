/* Asynchronous replication implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
 
/*
 *	异步replication实现
 */

#include "redis.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(int newfd);
void replicationSendAck(void);
void putSlaveOnline(redisClient *slave);

/* --------------------------- Utility functions ---------------------------- */
/*	通用函数的实现
 */
 
/* Return the pointer to a string representing the slave ip:listening_port
 * pair. Mostly useful for logging, since we want to log a slave using its
 * IP address and it's listening port which is more clear for the user, for
 * example: "Closing connection with slave 10.1.2.3:6380". */
/* 返回一个格式为slave ip:listening_port的string指针
 */
char *replicationGetSlaveName(redisClient *c) {
    static char buf[REDIS_PEER_ID_LEN];
    char ip[REDIS_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    if (anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1) {
        if (c->slave_listening_port)
            snprintf(buf,sizeof(buf),"%s:%d",ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-slave-port>",ip);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

/* ---------------------------------- MASTER -------------------------------- */
/* replication master端的实现
 */

//创建backlog
void createReplicationBacklog(void) {
    redisAssert(server.repl_backlog == NULL);
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    /* When a new backlog buffer is created, we increment the replication
     * offset by one to make sure we'll not be able to PSYNC with any
     * previous slave. This is needed because we avoid incrementing the
     * master_repl_offset if no backlog exists nor slaves are attached. */
    //每当新建一个backlog，对master的偏移量加一
    server.master_repl_offset++;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    //repl_backlog_off表示第一个复制字节的偏移位置
    server.repl_backlog_off = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * server.repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
//调整backlog的大小为newsize
void resizeReplicationBacklog(long long newsize) {
    if (newsize < REDIS_REPL_BACKLOG_MIN_SIZE)
        newsize = REDIS_REPL_BACKLOG_MIN_SIZE;
    if (server.repl_backlog_size == newsize) return;

    server.repl_backlog_size = newsize;
    if (server.repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. */
        zfree(server.repl_backlog);
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is empty. */
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}
//释放backlog的内存空间
void freeReplicationBacklog(void) {
    redisAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * server.master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the buffer. */
 //添加数据到replication backlog中去
void feedReplicationBacklog(void *ptr, size_t len) {
    unsigned char *p = ptr;

    server.master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. */
    while(len) {
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;
        if (thislen > len) thislen = len;
        memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);
        server.repl_backlog_idx += thislen;
        if (server.repl_backlog_idx == server.repl_backlog_size)
            server.repl_backlog_idx = 0;
        len -= thislen;
        p += thislen;
        server.repl_backlog_histlen += thislen;
    }
    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;
    /* Set the offset of the first byte we have in the backlog. */
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects
 * as input. */
//feedReplicationBacklog的一个包装函数
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[REDIS_LONGSTR_SIZE];
    void *p;
    size_t len;

    if (o->encoding == REDIS_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBacklog(p,len);
}
//把命令传递给每一个slave
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[REDIS_LONGSTR_SIZE];

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. */
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. */
    redisAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* Send SELECT command to every slave if needed. */
	//构造一条SELECT命令，添加到backlog中去
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        if (dictid >= 0 && dictid < REDIS_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(REDIS_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* Add the SELECT command into the backlog. */
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);

        /* Send it to slaves. */
		//把这条SELECT命令发送给每个slave
        listRewind(slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            addReply(slave,selectcmd);
        }

        if (dictid < 0 || dictid >= REDIS_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    server.slaveseldb = dictid;

    /* Write the command to the replication backlog if any. */
	//把命令写进backlog中去
    if (server.repl_backlog) {
        char aux[REDIS_LONGSTR_SIZE+3];

        /* Add the multi bulk reply length. */
        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBacklog(aux,len+3);

        for (j = 0; j < argc; j++) {
            long objlen = stringObjectLen(argv[j]);

            /* We need to feed the buffer with the object as a bulk reply
             * not just as a plain string, so create the $..CRLF payload len
             * and add the final CRLF */
            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);
            feedReplicationBacklogWithObject(argv[j]);
            feedReplicationBacklog(aux+len+1,2);
        }
    }

    /* Write the command to every slave. */
	//因为backlog是用来处理部分同步的，所以我们还要把命令写到每一个slave中去
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;

        /* Feed slaves that are waiting for the initial SYNC (so these commands
         * are queued in the output buffer until the initial SYNC completes),
         * or are already in sync with the master. */

        /* Add the multi bulk length. */
        addReplyMultiBulkLen(slave,argc);

        /* Finally any additional argument that was not stored inside the
         * static buffer if any (from j to argc). */
        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}
//把这条命令发给每个监视器
void replicationFeedMonitors(redisClient *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & REDIS_LUA_CLIENT) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & REDIS_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == REDIS_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(REDIS_STRING,cmdrepr);

    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        redisClient *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* Feed the slave 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. */
/* 把replication backlog中以offset开始到结束的数据feed给slave c
 */
long long addReplyReplicationBacklog(redisClient *c, long long offset) {
    long long j, skip, len;

    redisLog(REDIS_DEBUG, "[PSYNC] Slave request offset: %lld", offset);
	//repl_backlog_histlen为0表示backlog中没有历史数据
    if (server.repl_backlog_histlen == 0) {
        redisLog(REDIS_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    redisLog(REDIS_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    redisLog(REDIS_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    redisLog(REDIS_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    redisLog(REDIS_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);

    /* Compute the amount of bytes we need to discard. */
    skip = offset - server.repl_backlog_off;
    redisLog(REDIS_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Point j to the oldest byte, that is actaully our
     * server.repl_backlog_off byte. */
    //j表示在backlog中第一个复制的字节的位置
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    redisLog(REDIS_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* Discard the amount of data to seek to the specified 'offset'. */
    j = (j + skip) % server.repl_backlog_size;

    /* Feed slave with data. Since it is a circular buffer we have to
     * split the reply in two parts if we are cross-boundary. */
    len = server.repl_backlog_histlen - skip;
    redisLog(REDIS_DEBUG, "[PSYNC] Reply total length: %lld", len);
	//把len字节的数据传送给c
    while(len) {
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;

        redisLog(REDIS_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    return server.repl_backlog_histlen - skip;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return REDIS_OK, otherwise REDIS_ERR is returned and we proceed
 * with the usual full resync. */
 //这个函数从master的视角用来处理PSYNC命令
int masterTryPartialResynchronization(redisClient *c) {
    long long psync_offset, psync_len;
    char *master_runid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    /* Is the runid of this master the same advertised by the wannabe slave
     * via PSYNC? If runid changed this master is a different instance and
     * there is no way to continue. */
    //检查masterid是否相同
    if (strcasecmp(master_runid, server.runid)) {
        /* Run id "?" is used by slaves that want to force a full resync. */
        if (master_runid[0] != '?') {
            redisLog(REDIS_NOTICE,"Partial resynchronization not accepted: "
                "Runid mismatch (Client asked for runid '%s', my runid is '%s')",
                master_runid, server.runid);
        } else {
            redisLog(REDIS_NOTICE,"Full resync requested by slave %s",
                replicationGetSlaveName(c));
        }
		//如果不相同，则需要进行一次完全同步
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? */
	//获取slave偏移量
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       REDIS_OK) goto need_full_resync;
	//检查slave偏移量和server的全局偏移量之间的大小，如果满足下面三个条件之间的任何一个，则需要进行一次完全同步
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        redisLog(REDIS_NOTICE,
            "Unable to partial resync with slave %s for lack of backlog (Slave request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            redisLog(REDIS_WARNING,
                "Warning: slave %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a slave.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the slave. */
    /* 如果到达这里，则说明我们能够执行一次部分同步操作:
     * 1)设置client为slave
     * 2)以+CONTINUE通知client
     * 3)发送backlog数据给slave
     */
    c->flags |= REDIS_SLAVE;
    c->replstate = REDIS_REPL_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c);
    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * empty so this write will never fail actually. */
    buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
	//向slave回复+CONTINUE
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        return REDIS_OK;
    }
	//把部分同步的数据传送给slave
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    redisLog(REDIS_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at server.slaveseldb
     * to -1 to force the master to emit SELECT, since the slave already
     * has this state from the previous connection with the master. */

    refreshGoodSlavesCount();
    return REDIS_OK; /* The caller can return, no full resync needed. */

need_full_resync:
    /* We need a full resync for some reason... notify the client. */
    psync_offset = server.master_repl_offset;
    /* Add 1 to psync_offset if it the replication backlog does not exists
     * as when it will be created later we'll increment the offset by one. */
    if (server.repl_backlog == NULL) psync_offset++;
    /* Again, we can't use the connection buffers (see above). */
	//向slave回复+FULLRESYNC
    buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                      server.runid,psync_offset);
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        return REDIS_OK;
    }
    return REDIS_ERR;
}

/* Start a BGSAVE for replication goals, which is, selecting the disk or
 * socket target depending on the configuration, and making sure that
 * the script cache is flushed before to start.
 *
 * Returns REDIS_OK on success or REDIS_ERR otherwise. */
//开启一个BGSAVE为了replication任务
int startBgsaveForReplication(void) {
    int retval;

    redisLog(REDIS_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        server.repl_diskless_sync ? "slaves sockets" : "disk");

    if (server.repl_diskless_sync)
        retval = rdbSaveToSlavesSockets();
    else
        retval = rdbSaveBackground(server.rdb_filename);

    /* Flush the script cache, since we need that slave differences are
     * accumulated without requiring slaves to match our cached scripts. */
    if (retval == REDIS_OK) replicationScriptCacheFlush();
    return retval;
}

/* SYNC and PSYNC command implemenation. */
//SYNC和PSYNC命令都会调用这个函数，这个函数被master调用
void syncCommand(redisClient *c) {
    /* ignore SYNC if already slave or in monitor mode */
	//如果早已经是REDIS_SLAVE了说明同步早已经发生了
    if (c->flags & REDIS_SLAVE) return;

    /* Refuse SYNC requests if we are a slave but the link with our master
     * is not ok... */
    if (server.masterhost && server.repl_state != REDIS_REPL_CONNECTED) {
        addReplyError(c,"Can't SYNC while not connected with my master");
        return;
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    //如果server还有pending data没有传送给client，则SYNC和PSYNC命令不会执行，
    //因为我们需要一个干净的buffer用来存储BGSAVE和当前的dataset之间的异同，
    //以便我们能够copy给其他slaves
    if (listLength(c->reply) != 0 || c->bufpos != 0) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    redisLog(REDIS_NOTICE,"Slave %s asks for synchronization",
        replicationGetSlaveName(c));

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * +FULLRESYNC <runid> <offset>
     *
     * So the slave knows the new runid and offset to try a PSYNC later
     * if the connection with the master is lost. */
    /* 检查是否进行部分同步操作
     */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {	/* strcasecmp若两个字符相等返回0 */
        if (masterTryPartialResynchronization(c) == REDIS_OK) {
            server.stat_sync_partial_ok++;
            return; /* No full resync needed, return. */
        } else {
            char *master_runid = c->argv[1]->ptr;

            /* Increment stats for failed PSYNCs, but only if the
             * runid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not albe to partially
             * resync. */
            if (master_runid[0] != '?') server.stat_sync_partial_err++;
        }
    } else {
        /* If a slave uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like redis-cli --slave). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. */
        c->flags |= REDIS_PRE_PSYNC;
    }

    /* Full resynchronization. */
    server.stat_sync_full++;

    /* Here we need to check if there is a background saving operation
     * in progress, or if it is required to start one */
    //检查是否目前有一个子进程在执行RDB操作
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == REDIS_RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save. */
        redisClient *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
		//检查是否已经有slave正在进行同步操作了
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
        }
        if (ln) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            //表示已经有slave进行同步操作，那么从已经同步操作的这个slave的buffer里面把数据拷贝给当前的这个slave
            copyClientOutputBuffer(c,slave);
			//然后再把当前slave的状态改成等待后台的BGSAVE完成
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
            redisLog(REDIS_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. */
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
        }
    } else if (server.rdb_child_pid != -1 &&
               server.rdb_child_type == REDIS_RDB_CHILD_TYPE_SOCKET)
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. */
        c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
        redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
    } else {
        if (server.repl_diskless_sync) {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. */
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            if (server.repl_diskless_sync_delay)
                redisLog(REDIS_NOTICE,"Delay next BGSAVE for SYNC");
        } else {
            /* Ok we don't have a BGSAVE in progress, let's start one. */
            if (startBgsaveForReplication() != REDIS_OK) {
                redisLog(REDIS_NOTICE,"Replication failed, can't BGSAVE");
                addReplyError(c,"Unable to perform background save");
                return;
            }
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        }
    }

    if (server.repl_disable_tcp_nodelay)
        anetDisableTcpNoDelay(NULL, c->fd); /* Non critical if it fails. */
    c->repldbfd = -1;
    c->flags |= REDIS_SLAVE;
    server.slaveseldb = -1; /* Force to re-emit the SELECT command. */
    listAddNodeTail(server.slaves,c);
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL)
        createReplicationBacklog();
    return;
}

/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a slave in order to configure the replication
 * process before starting it with the SYNC command.
 *
 * Currently the only use of this command is to communicate to the master
 * what is the listening port of the Slave redis instance, so that the
 * master can accurately list slaves and their listening ports in
 * the INFO output.
 *
 * In the future the same command can be used in order to configure
 * the replication to initiate an incremental replication instead of a
 * full resync. */
/* REPLCONF命令的格式:
 *		REPLCONF <option> <value> <option> <value> ...
 *	这个命令被slave使用，用来配置复制过程在开始SYNC命令前
 *
 * 现在这个命令的唯一使用用途是和master节点进行通信，告诉master节点slave节点的监听端口，以便master能够
 * 精确地列出slaves和他们的监听端口在INFO输出中
 *
 * 在以后，这个命令能被用来初始化一个增量的复制来替代一个完全复制
 */
void replconfCommand(redisClient *c) {
    int j;
	//argc必定是2的整数倍
    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
	//处理每一个<option> <value>对
    for (j = 1; j < c->argc; j+=2) {
		//检测<option>是否是listening-port
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != REDIS_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {	/* 如果<option>是ack */
            /* REPLCONF ACK is used by slave to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. */
             
            /* REPLCONF ACK用来告诉master，slave目前已经处理的复制流的总数目，
             * 这个是一个内部命令，普通的clients应该从不使用
             */
            long long offset;

            if (!(c->flags & REDIS_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != REDIS_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            c->repl_ack_time = server.unixtime;
            /* If this was a diskless replication, we need to really put
             * the slave online when the first ACK is received (which
             * confirms slave is online and ready to get more data). */
             //如果下面两个条件成立，说明是diskless replication
            if (c->repl_put_online_on_ack && c->replstate == REDIS_REPL_ONLINE)
                putSlaveOnline(c);
            /* Note: this command does not reply anything! */
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. */
            //当前结点是slave才会执行这里，表明master节点发送了REPLCONF GETACK命令给当前slave节点，请求一个ACK
            if (server.masterhost && server.master) replicationSendAck();
            /* Note: this command does not reply anything! */
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

/* This function puts a slave in the online state, and should be called just
 * after a slave received the RDB file for the initial synchronization, and
 * we are finally ready to send the incremental stream of commands.
 *
 * It does a few things:
 *
 * 1) Put the slave in ONLINE state (useless when the function is called
 *    because state is already ONLINE but repl_put_online_on_ack is true).
 *	  (设置slave为ONLINE的状态)
 * 2) Make sure the writable event is re-installed, since calling the SYNC
 *    command disables it, so that we can accumulate output buffer without
 *    sending it to the slave.
 * 3) Update the count of good slaves. */
/* 这个函数设置一个slave成REDIS_REPL_ONLINE的状态，在一个slave接收到了RDB文件作为初始化同步之后应该被调用
 * 我们最终准备发送增加的命令给slave*/
void putSlaveOnline(redisClient *slave) {
	//REDIS_REPL_ONLINE表示slave节点和master节点之间的.rdb文件已经传输完了，接下来只需要传输变动的部分就可以了
    slave->replstate = REDIS_REPL_ONLINE;
    slave->repl_put_online_on_ack = 0;
    slave->repl_ack_time = server.unixtime; /* Prevent false timeout. */
	//注册可写事件，这个可写的事件是把还没写进slave的命令发送给slave
    if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
        sendReplyToClient, slave) == AE_ERR) {
        redisLog(REDIS_WARNING,"Unable to register writable event for slave bulk transfer: %s", strerror(errno));
        freeClient(slave);
        return;
    }
    refreshGoodSlavesCount();
    redisLog(REDIS_NOTICE,"Synchronization with slave %s succeeded",
        replicationGetSlaveName(slave));
}
//发送RDB文件给slave
void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *slave = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    char buf[REDIS_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble(序文，报头) as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". */
    if (slave->replpreamble) {
        nwritten = write(fd,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            redisLog(REDIS_VERBOSE,"Write error sending RDB preamble to slave: %s",
                strerror(errno));
            freeClient(slave);
            return;
        }
        server.stat_net_output_bytes += nwritten;
        sdsrange(slave->replpreamble,nwritten,-1);
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* fall through sending data. */
        } else {
            return;
        }
    }

    /* If the preamble was already transfered, send the RDB bulk data. */
	//发送RDB bulk data给slave
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,REDIS_IOBUF_LEN);
    if (buflen <= 0) {
        redisLog(REDIS_WARNING,"Read error sending DB to slave: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        if (errno != EAGAIN) {
            redisLog(REDIS_WARNING,"Write error sending DB to slave: %s",
                strerror(errno));
            freeClient(slave);
        }
        return;
    }
    slave->repldboff += nwritten;
    server.stat_net_output_bytes += nwritten;
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
		//重新这是slave的状态
        putSlaveOnline(slave);
    }
}

/* This function is called at the end of every background saving,
 * or when the replication RDB transfer strategy is modified from
 * disk to socket or the other way around.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization, and
 * to schedule a new BGSAVE if there are slaves that attached while a
 * BGSAVE was in progress, but it was not a good one for replication (no
 * other slave was accumulating differences).
 *
 * The argument bgsaveerr is REDIS_OK if the background saving succeeded
 * otherwise REDIS_ERR is passed to the function.
 * The 'type' argument is the type of the child that terminated
 * (if it had a disk or socket target). */
/* 这个函数在每次BGSAVE完成的最后被调用
 */
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    int startbgsave = 0;
    listIter li;

    listRewind(server.slaves,&li);
	//遍历每一个slave
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;
		//如果当前slave的replstate为REDIS_REPL_WAIT_BGSAVE_START，则马上开始一个BGSAVE
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            startbgsave = 1;
            slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            /* If this was an RDB on disk save, we have to prepare to send
             * the RDB from disk to the slave socket. Otherwise if this was
             * already an RDB -> Slaves socket transfer, used in the case of
             * diskless replication, our work is trivial, we can just put
             * the slave online. */
            if (type == REDIS_RDB_CHILD_TYPE_SOCKET) {
                redisLog(REDIS_NOTICE,
                    "Streamed RDB transfer with slave %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                /* Note: we wait for a REPLCONF ACK message from slave in
                 * order to really put it online (install the write handler
                 * so that the accumulated data can be transfered). However
                 * we change the replication state ASAP, since our slave
                 * is technically online now. */
                slave->replstate = REDIS_REPL_ONLINE;
                slave->repl_put_online_on_ack = 1;
                slave->repl_ack_time = server.unixtime; /* Timeout otherwise. */
            } else {
            	//如果bgsaveerr不等于REDIS_OK则说明BGSAVE出现问题了
                if (bgsaveerr != REDIS_OK) {
                    freeClient(slave);
                    redisLog(REDIS_WARNING,"SYNC failed. BGSAVE child returned an error");
                    continue;
                }
				//打开RDB文件
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClient(slave);
                    redisLog(REDIS_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                slave->repldboff = 0;
                slave->repldbsize = buf.st_size;
                slave->replstate = REDIS_REPL_SEND_BULK;
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) slave->repldbsize);

                aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
				//在当前slave的fd上注册可写事件
                if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                    freeClient(slave);
                    continue;
                }
            }
        }
    }
	//如果startbgsave为1，说明有slave还在等待一个BGSAVE，所以马上开始一个新的BGSAVE
    if (startbgsave) {
        if (startBgsaveForReplication() != REDIS_OK) {
            listIter li;

            listRewind(server.slaves,&li);
            redisLog(REDIS_WARNING,"SYNC failed. BGSAVE failed");
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;

                if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
            }
        }
    }
}

/* ----------------------------------- SLAVE -------------------------------- */

/* Abort the async download of the bulk dataset while SYNC-ing with master */
//中断当前slave节点和master节点之间的异步传输
void replicationAbortSyncTransfer(void) {
	//如果server.repl_state == REDIS_REPL_TRANSFER表明当前结点正在和master节点在进行数据传输
    redisAssert(server.repl_state == REDIS_REPL_TRANSFER);

    aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);
    close(server.repl_transfer_s);
    close(server.repl_transfer_fd);
    unlink(server.repl_transfer_tmpfile);
    zfree(server.repl_transfer_tmpfile);
    server.repl_state = REDIS_REPL_CONNECT;
}

/* Avoid the master to detect the slave is timing out(定时失效) while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entierly or
 * not, since the byte is indivisible(不能分割的).
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. */
/* 避免master去探测slave是定时失效的，当在初始化同步时正在装载RDB文件的时候。
 * 我们发送一个单一的换行符给master节点，这是有效的协议，但是能够保证这个单一字节完全发送出去，或者没有发送出去
 * 因为这个字节是不可分割的。
 * 这个函数在两种情况下被调用:
 *  (1)当我们用enptydb()来刷新目前的数据
 *  (2)当我们从master节点以一个RDB文件的方式来装载新数据
 */
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        if (write(server.repl_transfer_s,"\n",1) == -1) {
            /* Pinging back in this stage is best-effort. */
        }
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. */
void replicationEmptyDbCallback(void *privdata) {
    REDIS_NOTUSED(privdata);
    replicationSendNewlineToMaster();
}

/* Asynchronously read the SYNC payload we receive from a master */
//异步接收master的数据
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[4096];
    ssize_t nread, readlen;
    off_t left;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(privdata);
    REDIS_NOTUSED(mask);

    /* Static vars used to hold the EOF mark, and the last bytes received
     * form the server: when they match, we reached the end of the transfer. */
    static char eofmark[REDIS_RUN_ID_SIZE];
    static char lastbytes[REDIS_RUN_ID_SIZE];
    static int usemark = 0;

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. */
    if (server.repl_transfer_size == -1) {
        if (syncReadLine(fd,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            redisLog(REDIS_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }

        if (buf[0] == '-') {
            redisLog(REDIS_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            server.repl_transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            redisLog(REDIS_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored. */
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= REDIS_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,REDIS_RUN_ID_SIZE);
            memset(lastbytes,0,REDIS_RUN_ID_SIZE);
            /* Set any repl_transfer_size to avoid entering this code path
             * at the next call. */
            server.repl_transfer_size = 0;
            redisLog(REDIS_NOTICE,
                "MASTER <-> SLAVE sync: receiving streamed RDB from master");
        } else {
            usemark = 0;
            server.repl_transfer_size = strtol(buf+1,NULL,10);
            redisLog(REDIS_NOTICE,
                "MASTER <-> SLAVE sync: receiving %lld bytes from master",
                (long long) server.repl_transfer_size);
        }
        return;
    }

    /* Read bulk data */
    if (usemark) {
        readlen = sizeof(buf);
    } else {
        left = server.repl_transfer_size - server.repl_transfer_read;
        readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
    }

    nread = read(fd,buf,readlen);
    if (nread <= 0) {
        redisLog(REDIS_WARNING,"I/O error trying to sync with MASTER: %s",
            (nread == -1) ? strerror(errno) : "connection lost");
        replicationAbortSyncTransfer();
        return;
    }
    server.stat_net_input_bytes += nread;

    /* When a mark is used, we want to detect EOF asap in order to avoid
     * writing the EOF mark into the file... */
    int eof_reached = 0;

    if (usemark) {
        /* Update the last bytes array, and check if it matches our delimiter.*/
        if (nread >= REDIS_RUN_ID_SIZE) {
            memcpy(lastbytes,buf+nread-REDIS_RUN_ID_SIZE,REDIS_RUN_ID_SIZE);
        } else {
            int rem = REDIS_RUN_ID_SIZE-nread;
            memmove(lastbytes,lastbytes+nread,rem);
            memcpy(lastbytes+rem,buf,nread);
        }
        if (memcmp(lastbytes,eofmark,REDIS_RUN_ID_SIZE) == 0) eof_reached = 1;
    }

    server.repl_transfer_lastio = server.unixtime;
    if (write(server.repl_transfer_fd,buf,nread) != nread) {
        redisLog(REDIS_WARNING,"Write error or short write writing to the DB dump file needed for MASTER <-> SLAVE synchronization: %s", strerror(errno));
        goto error;
    }
    server.repl_transfer_read += nread;

    /* Delete the last 40 bytes from the file if we reached EOF. */
    if (usemark && eof_reached) {
        if (ftruncate(server.repl_transfer_fd,
            server.repl_transfer_read - REDIS_RUN_ID_SIZE) == -1)
        {
            redisLog(REDIS_WARNING,"Error truncating the RDB file received from the master for SYNC: %s", strerror(errno));
            goto error;
        }
    }

    /* Sync data on disk from time to time, otherwise at the end of the transfer
     * we may suffer a big delay as the memory buffers are copied into the
     * actual disk. */
    if (server.repl_transfer_read >=
        server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
    {
        off_t sync_size = server.repl_transfer_read -
                          server.repl_transfer_last_fsync_off;
        rdb_fsync_range(server.repl_transfer_fd,
            server.repl_transfer_last_fsync_off, sync_size);
        server.repl_transfer_last_fsync_off += sync_size;
    }

    /* Check if the transfer is now complete */
    if (!usemark) {
        if (server.repl_transfer_read == server.repl_transfer_size)
            eof_reached = 1;
    }

    if (eof_reached) {
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            redisLog(REDIS_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
            replicationAbortSyncTransfer();
            return;
        }
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Flushing old data");
        signalFlushedDb(-1);
        emptyDb(replicationEmptyDbCallback);
        /* Before loading the DB into memory we need to delete the readable
         * handler, otherwise it will get called recursively since
         * rdbLoad() will call the event loop to process events from time to
         * time for non blocking loading. */
        aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Loading DB in memory");
        if (rdbLoad(server.rdb_filename) != REDIS_OK) {
            redisLog(REDIS_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
            replicationAbortSyncTransfer();
            return;
        }
        /* Final setup of the connected slave <- master link */
        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);
        server.master = createClient(server.repl_transfer_s);
        server.master->flags |= REDIS_MASTER;
        server.master->authenticated = 1;
        server.repl_state = REDIS_REPL_CONNECTED;
        server.master->reploff = server.repl_master_initial_offset;
        memcpy(server.master->replrunid, server.repl_master_runid,
            sizeof(server.repl_master_runid));
        /* If master offset is set to -1, this master is old and is not
         * PSYNC capable, so we flag it accordingly. */
        if (server.master->reploff == -1)
            server.master->flags |= REDIS_PRE_PSYNC;
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Finished with success");
        /* Restart the AOF subsystem now that we finished the sync. This
         * will trigger an AOF rewrite, and when done will start appending
         * to the new file. */
        if (server.aof_state != REDIS_AOF_OFF) {
            int retry = 10;

            stopAppendOnly();
            while (retry-- && startAppendOnly() == REDIS_ERR) {
                redisLog(REDIS_WARNING,"Failed enabling the AOF after successful master synchronization! Trying it again in one second.");
                sleep(1);
            }
            if (!retry) {
                redisLog(REDIS_WARNING,"FATAL: this slave instance finished the synchronization with its master, but the AOF can't be turned on. Exiting now.");
                exit(1);
            }
        }
    }

    return;

error:
    replicationAbortSyncTransfer();
    return;
}

/* Send a synchronous command to the master. Used to send AUTH and
 * REPLCONF commands before starting the replication with SYNC.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
//发送一个同步命令给master。在开始一个SYNC同步之前被AUTH和REPLCONF命令所使用
//返回命令的回复，如果是错误返回，则返回的字符串的第一个字节是"-"
char *sendSynchronousCommand(int fd, ...) {
    va_list ap;
    sds cmd = sdsempty();
    char *arg, buf[256];

    /* Create the command to send to the master, we use simple inline
     * protocol for simplicity as currently we only send simple strings. */
    //构造一条发送给master的命令，这个命令使用的是简单的inline protocol来构造的
    va_start(ap,fd);
    while(1) {
        arg = va_arg(ap, char*);
        if (arg == NULL) break;

        if (sdslen(cmd) != 0) cmd = sdscatlen(cmd," ",1);
        cmd = sdscat(cmd,arg);
    }
    cmd = sdscatlen(cmd,"\r\n",2);

    /* Transfer command to the server. */
	//发送命令给fd
    if (syncWrite(fd,cmd,sdslen(cmd),server.repl_syncio_timeout*1000) == -1) {
        sdsfree(cmd);
        return sdscatprintf(sdsempty(),"-Writing to master: %s",
                strerror(errno));
    }
    sdsfree(cmd);

    /* Read the reply from the server. */
	//读取命令的回复
    if (syncReadLine(fd,buf,sizeof(buf),server.repl_syncio_timeout*1000) == -1)
    {
        return sdscatprintf(sdsempty(),"-Reading from master: %s",
                strerror(errno));
    }
    return sdsnew(buf);
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master run id and the master replication
 * global offset.
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 1) We pass the function an already connected socket "fd".
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the server.master client structure.
 *
 * The function returns:
 *
 * PSYNC_CONTINUE: If the PSYNC command succeded and we can continue.
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master run_id and global replication
 *                   offset is saved.
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 */

#define PSYNC_CONTINUE 0
#define PSYNC_FULLRESYNC 1
#define PSYNC_NOT_SUPPORTED 2

/* slave尝试进行一次部分同步
 * 如果返回的是PSYNC_CONTINUE，说明可以进行部分同步操作
 */
int slaveTryPartialResynchronization(int fd) {
    char *psync_runid;
    char psync_offset[32];
    sds reply;

    /* Initially set repl_master_initial_offset to -1 to mark the current
     * master run_id and offset as not valid. Later if we'll be able to do
     * a FULL resync using the PSYNC command we'll set the offset at the
     * right value, so that this information will be propagated to the
     * client structure representing the master into server.master. */
    server.repl_master_initial_offset = -1;
	//如果先前有cached master，则使用先前master的runid和偏移量，replrunid和reploff保存着slave和master同步的信息
    if (server.cached_master) {
        psync_runid = server.cached_master->replrunid;
        snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
        redisLog(REDIS_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_runid, psync_offset);
    } else {
        redisLog(REDIS_NOTICE,"Partial resynchronization not possible (no cached master)");
        psync_runid = "?";
        memcpy(psync_offset,"-1",3);
    }

    /* Issue the PSYNC command */
	//发送PSYNC命令给master
    reply = sendSynchronousCommand(fd,"PSYNC",psync_runid,psync_offset,NULL);
	
	//检查master的回复，如果为FULLRESYNC，说明要进行一次完全同步
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *runid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the run id
         * and the replication offset. */
        runid = strchr(reply,' ');
        if (runid) {
            runid++;
            offset = strchr(runid,' ');
            if (offset) offset++;
        }
        if (!runid || !offset || (offset-runid-1) != REDIS_RUN_ID_SIZE) {
            redisLog(REDIS_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * runid to make sure next PSYNCs will fail. */
            memset(server.repl_master_runid,0,REDIS_RUN_ID_SIZE+1);
        } else {
            memcpy(server.repl_master_runid, runid, offset-runid-1);
            server.repl_master_runid[REDIS_RUN_ID_SIZE] = '\0';
            server.repl_master_initial_offset = strtoll(offset,NULL,10);
            redisLog(REDIS_NOTICE,"Full resync from master: %s:%lld",
                server.repl_master_runid,
                server.repl_master_initial_offset);
        }
        /* We are going to full resync, discard the cached master structure. */
        replicationDiscardCachedMaster();
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }
	
	//如果回复等于CONTINUE，说明可以进行一次部分同步操作
    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted, set the replication state accordingly */
        redisLog(REDIS_NOTICE,
            "Successful partial resynchronization with master.");
        sdsfree(reply);
        replicationResurrectCachedMaster(fd);
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we receied either an error since the master does
     * not understand PSYNC, or an unexpected reply from the master.
     * Return PSYNC_NOT_SUPPORTED to the caller in both cases. */

    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        redisLog(REDIS_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        redisLog(REDIS_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster();
    return PSYNC_NOT_SUPPORTED;
}

void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    char tmpfile[256], *err;
    int dfd, maxtries = 5;
    int sockerr = 0, psync_result;
    socklen_t errlen = sizeof(sockerr);
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(privdata);
    REDIS_NOTUSED(mask);

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. */
    if (server.repl_state == REDIS_REPL_NONE) {
        close(fd);
        return;
    }

    /* Check for errors in the socket. */
	//检查连接master的这个socket有没有错误
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
        redisLog(REDIS_WARNING,"Error condition on socket for SYNC: %s",
            strerror(sockerr));
        goto error;
    }

    /* If we were connecting, it's time to send a non blocking PING, we want to
     * make sure the master is able to reply before going into the actual
     * replication process where we have long timeouts in the order of
     * seconds (in the meantime the slave would block). */
    /* 如果repl_state==REDIS_REPL_CONNECTING，首先发送一个PING命令给master，用来
     * 确定master节点在进行实际的replication之前是否能够回复slave的请求
     */
    if (server.repl_state == REDIS_REPL_CONNECTING) {
        redisLog(REDIS_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. */
        aeDeleteFileEvent(server.el,fd,AE_WRITABLE);
        server.repl_state = REDIS_REPL_RECEIVE_PONG;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. */
        //发送PING给master
        syncWrite(fd,"PING\r\n",6,100);
        return;
    }

    /* Receive the PONG command. */
    if (server.repl_state == REDIS_REPL_RECEIVE_PONG) {
        char buf[1024];

        /* Delete the readable event, we no longer need it now that there is
         * the PING reply to read. */
        aeDeleteFileEvent(server.el,fd,AE_READABLE);

        /* Read the reply with explicit timeout. */
        buf[0] = '\0';
		//从fd中读取数据
        if (syncReadLine(fd,buf,sizeof(buf),
            server.repl_syncio_timeout*1000) == -1)
        {
            redisLog(REDIS_WARNING,
                "I/O error reading PING reply from master: %s",
                strerror(errno));
            goto error;
        }

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. */
        //检查返回的数据是否是我们希望的
        if (buf[0] != '+' &&
            strncmp(buf,"-NOAUTH",7) != 0 &&
            strncmp(buf,"-ERR operation not permitted",28) != 0)
        {
            redisLog(REDIS_WARNING,"Error reply to PING from master: '%s'",buf);
            goto error;
        } else {
            redisLog(REDIS_NOTICE,
                "Master replied to PING, replication can continue...");
        }
    }

    /* AUTH with the master if required. */
	//如果master要求授权，首先进行密码验证
    if(server.masterauth) {
        err = sendSynchronousCommand(fd,"AUTH",server.masterauth,NULL);
        if (err[0] == '-') {
            redisLog(REDIS_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
    }

    /* Set the slave port, so that Master's INFO command can list the
     * slave listening port correctly. */
    //设置slave的port，发送给master
    {
        sds port = sdsfromlonglong(server.port);
        err = sendSynchronousCommand(fd,"REPLCONF","listening-port",port,
                                         NULL);
        sdsfree(port);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        if (err[0] == '-') {
            redisLog(REDIS_NOTICE,"(Non critical) Master does not understand REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
    }

    /* Try a partial resynchonization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master run id
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. */
    //尝试进行一次部分同步。
    psync_result = slaveTryPartialResynchronization(fd);
	//如果master节点返回的是PSYNC_CONTINUE，说明可以进行部分同步操作
    if (psync_result == PSYNC_CONTINUE) {
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Master accepted a Partial Resynchronization.");
        return;
    }

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the server.repl_master_runid and repl_master_initial_offset are
     * already populated. */
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        redisLog(REDIS_NOTICE,"Retrying with SYNC...");
		//发送SYNC命令给master
        if (syncWrite(fd,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            redisLog(REDIS_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* Prepare a suitable temp file for bulk transfer */
	//新建一个临时文件用来和msater进行.rdb文件数据传输之用
    while(maxtries--) {
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        redisLog(REDIS_WARNING,"Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        goto error;
    }

    /* Setup the non blocking download of the bulk file. */
	//注册可读的事件
    if (aeCreateFileEvent(server.el,fd, AE_READABLE,readSyncBulkPayload,NULL)
            == AE_ERR)
    {
        redisLog(REDIS_WARNING,
            "Can't create readable event for SYNC: %s (fd=%d)",
            strerror(errno),fd);
        goto error;
    }

    server.repl_state = REDIS_REPL_TRANSFER;
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_fd = dfd;
    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_tmpfile = zstrdup(tmpfile);
    return;

error:
    close(fd);
    server.repl_transfer_s = -1;
    server.repl_state = REDIS_REPL_CONNECT;
    return;
}

//连接master
int connectWithMaster(void) {
    int fd;
	//连接master
    fd = anetTcpNonBlockBestEffortBindConnect(NULL,
        server.masterhost,server.masterport,REDIS_BIND_ADDR);
    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
	//注册事件
    if (aeCreateFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE,syncWithMaster,NULL) ==
            AE_ERR)
    {
        close(fd);
        redisLog(REDIS_WARNING,"Can't create readable event for SYNC");
        return REDIS_ERR;
    }

    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_s = fd;
    server.repl_state = REDIS_REPL_CONNECTING;
    return REDIS_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it. */
//当一个non blocking connection正在进行中时，这个函数被调用会撤销正在连接的状态
void undoConnectWithMaster(void) {
    int fd = server.repl_transfer_s;

    redisAssert(server.repl_state == REDIS_REPL_CONNECTING ||
                server.repl_state == REDIS_REPL_RECEIVE_PONG);
    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    close(fd);
    server.repl_transfer_s = -1;
    server.repl_state = REDIS_REPL_CONNECT;
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REDIS_REPL_CONNECT.
 *
 * Otherwise zero is returned and no operation is perforemd at all. */
//取消replication:取消non-blocking连接或者bulk transfer
int cancelReplicationHandshake(void) {
    if (server.repl_state == REDIS_REPL_TRANSFER) {
        replicationAbortSyncTransfer();
    } else if (server.repl_state == REDIS_REPL_CONNECTING ||
             server.repl_state == REDIS_REPL_RECEIVE_PONG)
    {
        undoConnectWithMaster();
    } else {
        return 0;
    }
    return 1;
}

/* Set replication to the specified master address and port. */
//设置master地址和端口号，如果调用了这个函数，说明当前结点要变成slave了，或者说改变当前结点的master节点了
void replicationSetMaster(char *ip, int port) {
    sdsfree(server.masterhost);
    server.masterhost = sdsnew(ip);
    server.masterport = port;
    if (server.master) freeClient(server.master);
    disconnectAllBlockedClients(); /* Clients blocked in master, now slave.(当前结点由master变成slave了) */
    disconnectSlaves(); /* Force our slaves to resync with us as well. */
    replicationDiscardCachedMaster(); /* Don't try a PSYNC. */
    freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. */
    cancelReplicationHandshake();
    server.repl_state = REDIS_REPL_CONNECT;
    server.master_repl_offset = 0;
    server.repl_down_since = 0;
}

/* Cancel replication, setting the instance as a master itself. */
//取消replication，设置当前实例为master
void replicationUnsetMaster(void) {
	//如果当前结点不是slave，则直接返回什么都不用做
    if (server.masterhost == NULL) return; /* Nothing to do. */
    sdsfree(server.masterhost);
    server.masterhost = NULL;
    if (server.master) {
        if (listLength(server.slaves) == 0) {
            /* If this instance is turned into a master and there are no
             * slaves, it inherits the replication offset from the master.
             * Under certain conditions this makes replicas comparable by
             * replication offset to understand what is the most updated. */
			/* 如果当前实例变为master，并且没有slaves，那么从master中继承replication偏移量
			 */
			server.master_repl_offset = server.master->reploff;
            freeReplicationBacklog();
        }
        freeClient(server.master);
    }
    replicationDiscardCachedMaster();
    cancelReplicationHandshake();
    server.repl_state = REDIS_REPL_NONE;
}

//SLAVEOF命令的实现函数:把当前结点变成slave节点
void slaveofCommand(redisClient *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. */
    if (server.cluster_enabled) {
        addReplyError(c,"SLAVEOF not allowed in cluster mode.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. */
    //如果是"NO" "ONE"则把当前实例变成一个master
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            replicationUnsetMaster();
            redisLog(REDIS_NOTICE,"MASTER MODE enabled (user request)");
        }
    } else {
        long port;
		//从client中取得端口号参数
        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != REDIS_OK))
            return;

        /* Check if we are already attached to the specified slave */
		//检查我们当前要求连接的master和当前已经连接上的master是否重复
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            redisLog(REDIS_NOTICE,"SLAVE OF would result into synchronization with the master we are already connected with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified master\r\n"));
            return;
        }
        /* There was no previous master or the user specified a different one,
         * we can continue. */
        //设置slave节点中有关master的一些属性，设置完之后，当前节点就变成slave节点状态了
        replicationSetMaster(c->argv[1]->ptr, port);
        redisLog(REDIS_NOTICE,"SLAVE OF %s:%d enabled (user request)",
            server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or slave) and additional information related to replication
 * in an easy to process format. */
/* ROLE命令:提供实例角色(master或slave)的信息和有关复制的额外信息，以一种简单的处理格式提供
 */
void roleCommand(redisClient *c) {
	//如果masterhost==NULL说明当前实例是master节点
    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyMultiBulkLen(c,3);
		//传送"master"字符信息给client
        addReplyBulkCBuffer(c,"master",6);
		//传送偏移量给client
        addReplyLongLong(c,server.master_repl_offset);
        mbcount = addDeferredMultiBulkLength(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            char ip[REDIS_IP_STR_LEN];

            if (anetPeerToString(slave->fd,ip,sizeof(ip),NULL) == -1) continue;
            if (slave->replstate != REDIS_REPL_ONLINE) continue;
            addReplyMultiBulkLen(c,3);
            addReplyBulkCString(c,ip);
            addReplyBulkLongLong(c,slave->slave_listening_port);
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredMultiBulkLength(c,mbcount,slaves);
    } else {
        char *slavestate = NULL;

        addReplyMultiBulkLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        addReplyBulkCString(c,server.masterhost);
        addReplyLongLong(c,server.masterport);
        switch(server.repl_state) {
        case REDIS_REPL_NONE: slavestate = "none"; break;
        case REDIS_REPL_CONNECT: slavestate = "connect"; break;
        case REDIS_REPL_CONNECTING: slavestate = "connecting"; break;
        case REDIS_REPL_RECEIVE_PONG: /* see next */
        case REDIS_REPL_TRANSFER: slavestate = "sync"; break;
        case REDIS_REPL_CONNECTED: slavestate = "connected"; break;
        default: slavestate = "unknown"; break;
        }
        addReplyBulkCString(c,slavestate);
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. */
//发送一个REPLCONF ACK命令给master，告诉master关于自己目前处理的偏移量。
void replicationSendAck(void) {
    redisClient *c = server.master;

    if (c != NULL) {
        c->flags |= REDIS_MASTER_FORCE_REPLY;
        addReplyMultiBulkLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        c->flags &= ~REDIS_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC -------------------------- */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * 为了实现部分同步，在一个短暂的断开状态之后我们要cache我们的master的client结构
 * It is cached into server.cached_master and flushed away(冲去) using the following
 * functions. */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destryoing it. (这个函数被freeclient函数调用为了cache master而不是直接销毁它)freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate(使恢复活动) the cached master.
 */
void replicationCacheMaster(redisClient *c) {
    listNode *ln;

    redisAssert(server.master != NULL && server.cached_master == NULL);
    redisLog(REDIS_NOTICE,"Caching the disconnected master state.");

    /* Remove from the list of clients, we don't want this client to be
     * listed by CLIENT LIST or processed in any way by batch operations. */
    ln = listSearchKey(server.clients,c);
    redisAssert(ln != NULL);
	//在server.clients中删除客户端c
    listDelNode(server.clients,ln);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). */
     //把master缓存起来
    server.cached_master = server.master;

    /* Remove the event handlers and close the socket. We'll later reuse
     * the socket of the new connection with the master during PSYNC. */
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    close(c->fd);

    /* Set fd to -1 so that we can safely call freeClient(c) later. */
    c->fd = -1;

    /* Invalidate(使无效) the Peer ID cache. */
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. */
    replicationHandleMasterDisconnection();
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. */
//释放掉cached master，这个函数当不存在部分同步或者重连的情况下被调用(这两种情况下意味着不需要cached master了)
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;

    redisLog(REDIS_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~REDIS_MASTER;
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * This function is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from were this
 * master left. */
//把cached master变为目前的master，使用传进来的文件描述符newfd作为新master的socket
void replicationResurrectCachedMaster(int newfd) {
    server.master = server.cached_master;
    server.cached_master = NULL;
	//设置master的fd为传进来的newfd
    server.master->fd = newfd;
    server.master->flags &= ~(REDIS_CLOSE_AFTER_REPLY|REDIS_CLOSE_ASAP);
    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;
    server.repl_state = REDIS_REPL_CONNECTED;

    /* Re-add to the list of clients. */
	//重新把master添加到clients链表中
    listAddNodeTail(server.clients,server.master);
	//注册newfd的读事件
    if (aeCreateFileEvent(server.el, newfd, AE_READABLE,
                          readQueryFromClient, server.master)) {
        redisLog(REDIS_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. */
     //如果在server.master中有阻塞的数据，则注册一个写事件
    if (server.master->bufpos || listLength(server.master->reply)) {
        if (aeCreateFileEvent(server.el, newfd, AE_WRITABLE,
                          sendReplyToClient, server.master)) {
            redisLog(REDIS_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag(延迟，滞后) <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). */
/* 这个函数对lag<=min-slaves-max-lag的slave进行统计
 */
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;
	//如果repl_min_slaves_to_write和repl_min_slaves_max_lag两个都没被设置，则直接返回
    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;
	//遍历每一个slave
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;
		//从下面计算lag的方法，可以看出lag指的是slave当前时间距离上次被处理的时间之间的间隔
        time_t lag = server.unixtime - slave->repl_ack_time;

        if (slave->replstate == REDIS_REPL_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
	//更新repl_good_slaves_count
    server.repl_good_slaves_count = good;
}

/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected slave, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * We don't care about taking a different cache for every different slave
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is trasmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * This is how the system works:
 *
 * 1) Every time a new slave connects, we flush the whole script cache.
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 * 3) Every time we trasmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    slave knows about the script starting from now.
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 * 5) When the last slave disconnects, flush the cache.
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 */

/* Initialize the script cache, only called at startup. */
void replicationScriptCacheInit(void) {
    server.repl_scriptcache_size = 10000;
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    server.repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every slave knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 1) Every time a new slave reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 * 2) Every time an AOF rewrite is performed.
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 */
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. */
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. */
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);

        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        redisAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }

    /* Add current. */
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    redisAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. */
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 下面是Redis同步复制设计的摘要要点
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 * - Redis master有一个全局的复制偏移量，被PSYNC使用
 * - Master每次有一个新的命令发送给了slaves，就会对复制偏移量加一
 * - Slaves回复master目前为止被处理过的偏移量
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas(复制) that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.(这个命令返回我们最终处理查询的复制的数量，最终至少num_replicas或者时间超时)
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.(每次一个client处理了一条命令，我们记住这个
 *   复制偏移量在发送这个命令给slaves之后)
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).(当WAIT被调用之后，我们要求slaves尽可能快的发
 *   送一个ack，于此同时cient被阻塞)
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.(一旦对于给定的偏移量我们收到足够的ACKs或者时间超时，WAIT命令解除阻塞，发送回复给client)
 */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronouns replication
 * in a given event loop iteration, and send a single GETACK for them all. */
 //设置一个标志以便我们能广播一个REOLCONF GETACK命令给所有的slaves在beforeSleep函数调用之前。
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
//返回早已经回复了特定复制偏移量的slave的数量，
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;
	//遍历每一个slave
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;
		//如果replstate!=REDIS_REPL_ONLINE那说明至少.rdb文件还没传输完成，而ack是关于和命令相关的偏移量
        if (slave->replstate != REDIS_REPL_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
//WAIT N个复制量的确认回复
void waitCommand(redisClient *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    /* Argument parsing. */
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != REDIS_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != REDIS_OK) return;

    /* First try without blocking at all. */
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & REDIS_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,REDIS_BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    //确保进入事件循环前server能够发送一个ACK请求给所有的slaves，这样slave接到这个请求后就会发送带有偏移量的回复给server
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
//把client c从阻塞链表中移除
void unblockClientWaitingReplicas(redisClient *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
//检测我们是否有client阻塞在WAIT命令上，如果有并且我们收到了足够多的ACKs从slaves，那么把client变为非阻塞
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;
	//遍历阻塞链表
    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        redisClient *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
//返回slave的复制偏移量，这个偏移量是我们早已经处理的master复制流中的偏移量
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */

/* Replication cron function, called 1 time per second. */
//replication cron函数，每一秒钟调用一次
//这个函数用来重连master节点和检测传输故障
void replicationCron(void) {
	//如果server.masterhost存在表明目前节点是slave节点

    /* Non blocking connection timeout? */
	//检测是否超时连接了
    if (server.masterhost &&
        (server.repl_state == REDIS_REPL_CONNECTING ||
         server.repl_state == REDIS_REPL_RECEIVE_PONG) &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"Timeout connecting to the MASTER...");
        undoConnectWithMaster();
    }

    /* Bulk transfer I/O timeout? */
	//检测数据传输中是否超时了
    if (server.masterhost && server.repl_state == REDIS_REPL_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        replicationAbortSyncTransfer();
    }

    /* Timed out master when we are an already connected slave? */
	//当前结点是一个早连上了master的slave节点，检测是不是很久没和master进行活动了
    if (server.masterhost && server.repl_state == REDIS_REPL_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"MASTER timeout: no data nor PING received...");
        freeClient(server.master);
    }

    /* Check if we should connect to a MASTER */
	//检测我们是否应该连接一个master
    if (server.repl_state == REDIS_REPL_CONNECT) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        if (connectWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync started");
        }
    }

    /* Send ACK to master from time to time(不时，有时，偶尔).
     * Note that we do not send periodic(周期性的，周期的) acks to masters that don't
     * support PSYNC and replication offsets. */
     //如果不支持PSYNC，我们调用replicationSendAck发送ACK给master
    if (server.masterhost && server.master &&
        !(server.master->flags & REDIS_PRE_PSYNC))
        replicationSendAck();

    /* If we have attached slaves, PING them from time to time.
     * So slaves can implement an explicit timeout to masters, and will
     * be able to detect a link disconnection even if the TCP connection
     * will not actually go down. */
    /* 如果当前结点有关联的slaves，那么时不时的PING他们。
     * server.repl_ping_slave_period * server.hz表示的是每serverCron这个函数运行了这么多次就PING一下slaves
     */
    if (!(server.cronloops % (server.repl_ping_slave_period * server.hz))) {
        listIter li;
        listNode *ln;
        robj *ping_argv[1];

        /* First, send PING */
		//首先，发送PING命令
        ping_argv[0] = createStringObject("PING",4);
		//发送给slaves
        replicationFeedSlaves(server.slaves, server.slaveseldb, ping_argv, 1);
        decrRefCount(ping_argv[0]);

        /* Second, send a newline to all the slaves in pre-synchronization
         * stage, that is, slaves waiting for the master to create the RDB file.
         * The newline will be ignored by the slave but will refresh the
         * last-io timer preventing a timeout. */
        //然后发送一个换行符给所有的处于预同步阶段的slaves，在这个阶段的slaves正在等待在创建RDB文件的master节点
        //这个换行符会被slave忽略，但是会刷新last-io的值，阻止slave超时
        listRewind(server.slaves,&li);
		//遍历每一个slave
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;

            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START ||
                (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END &&
                 server.rdb_child_type != REDIS_RDB_CHILD_TYPE_SOCKET))
            {
                if (write(slave->fd, "\n", 1) == -1) {
                    /* Don't worry, it's just a ping. */
                }
            }
        }
    }

    /* Disconnect timedout slaves. */
	//断开超时的slaves
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;

            if (slave->replstate != REDIS_REPL_ONLINE) continue;
			//如果当前slave不支持PSYNC，则跳过(因为断开这种slave没有实际意义)
            if (slave->flags & REDIS_PRE_PSYNC) continue;
            if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout)
            {
                redisLog(REDIS_WARNING, "Disconnecting timedout slave: %s",
                    replicationGetSlaveName(slave));
                freeClient(slave);
            }
        }
    }

    /* If we have no attached slaves and there is a replication backlog
     * using memory, free it after some (configured) time. */
    /* 如果我们没有关联的slaves，但是有一个replication backlog，则在一些事件后释放这个backlog
     */
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;

		//如果没有slaves的时间超出了设置的时间，则释放这个backlog
        if (idle > server.repl_backlog_time_limit) {
            freeReplicationBacklog();
            redisLog(REDIS_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected slaves.",
                (int) server.repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
     * free our Replication Script Cache as there is no need to propagate
     * EVALSHA at all. */
    if (listLength(server.slaves) == 0 &&
        server.aof_state == REDIS_AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* If we are using diskless replication and there are slaves waiting
     * in WAIT_BGSAVE_START state, check if enough seconds elapsed and
     * start a BGSAVE.
     *
     * This code is also useful to trigger a BGSAVE if the diskless
     * replication was turned off with CONFIG SET, while there were already
     * slaves in WAIT_BGSAVE_START state. */
    /* 如果我们正在使用diskless replication，并且这里有slaves正在以WAIT_BGSAVE_START等待，
     * 检查是否已经等待了足够的时间，然后开启一个BGSAVE
     *
     * 这段代码对于触发一个BGSAVE很有帮助，如果diskless replication被CONFIG_SET关闭了，于此同时还有slave早就
     * 处于WAIT_BGSAVE_START状态了
     */
     //rdb_child_pid==-1和aof_child_pid==-1说明此时没有子进程在进行BGSAVE操作
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        listNode *ln;
        listIter li;
		
		//遍历每一个slave，取得最大空闲时间max_idle
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
                idle = server.unixtime - slave->lastinteraction;
                if (idle > max_idle) max_idle = idle;
                slaves_waiting++;
            }
        }

        if (slaves_waiting && max_idle > server.repl_diskless_sync_delay) {
            /* Start a BGSAVE. Usually with socket target, or with disk target
             * if there was a recent socket -> disk config change. */
            //开启一个BGSAVE操作
            if (startBgsaveForReplication() == REDIS_OK) {
                /* It started! We need to change the state of slaves
                 * from WAIT_BGSAVE_START to WAIT_BGSAVE_END in case
                 * the current target is disk. Otherwise it was already done
                 * by rdbSaveToSlavesSockets() which is called by
                 * startBgsaveForReplication(). */
                listRewind(server.slaves,&li);
                while((ln = listNext(&li))) {
                    redisClient *slave = ln->value;
                    if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                        slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
                }
            }
        }
    }

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    refreshGoodSlavesCount();
}
