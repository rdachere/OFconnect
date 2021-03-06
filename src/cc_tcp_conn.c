/*
*****************************************************
**      CodeChix ONF Driver (LibCCOF)
**      codechix.org - May the code be with you...
**              Sept. 15, 2013
*****************************************************
**
** License:        Apache 2.0 (ONF requirement)
** Version:        0.0
** LibraryName:    LibCCOF
** GLIB License:   GNU LGPL
** Description:    TCP implementation for LibCCOF
** Assumptions:    N/A
** Testing:        N/A
** Authors:        Ramya Bolla
**
*****************************************************
*/

#include "cc_of_global.h"
#include "cc_of_priv.h"
#include <unistd.h>
#include <fcntl.h>

// Number of backlog connection requests
#define LISTENQ 1024

/* Forward Declarations */
cc_of_ret tcp_open_clientfd(cc_ofdev_key_t key, cc_ofchannel_key_t ofchann_key);
cc_of_ret tcp_open_listenfd(cc_ofdev_key_t key);
cc_of_ret tcp_accept(int listenfd, cc_ofdev_key_t key);
cc_of_ret tcp_close(int sockfd);
ssize_t tcp_read(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr UNUSED, 
                 socklen_t *addrlen UNUSED);
ssize_t tcp_write(int sockfd, const void *buf, size_t len, int flags,
                  const struct sockaddr *dest_addr UNUSED,
                  socklen_t addrlen UNUSED);
    

/* Callback Registration for TCP */
net_svcs_t tcp_sockfns = {
    tcp_open_clientfd,
    tcp_open_listenfd,
    tcp_accept,
    tcp_close,
    tcp_read,
    tcp_write,
};

extern net_svcs_t tcp_sockfns;


void process_listenfd_pollin_func(char *tname UNUSED,
                                  adpoll_fd_info_t *data_p,
                                  adpoll_send_msg_htbl_info_t *unused_data UNUSED)
{
    int listenfd;    
    gboolean found = FALSE;
    
    if (!data_p) {
        CC_LOG_INFO("%s(%d): %s", __FUNCTION__, __LINE__,
                    "Invalid data passed to listenfd callback."
                    "Cannot accept new connection.");
        return;
    }
    
    listenfd = data_p->fd;

    /* 
     * Do a reverse lookup to get the dev_key 
     * corresponding to this listenfd
     */
    GHashTableIter ofdev_iter;
    cc_ofdev_key_t *dev_key;
    cc_ofdev_info_t *dev_info;
    cc_ofdev_key_t dkey;
    
    g_mutex_lock(&cc_of_global.ofdev_htbl_lock);
    
    g_hash_table_iter_init(&ofdev_iter, cc_of_global.ofdev_htbl);
    while (g_hash_table_iter_next(&ofdev_iter, (gpointer *)&dev_key, (gpointer *)&dev_info)) {
        if (dev_info->main_sockfd_tcp == listenfd) {
            // Call NetSVCS Accept
            /* Unlock ofdev_htbl here as accept locks it again.
             * TODO: check if we could avoid this
             */

            CC_LOG_DEBUG("%s(%d):, Calling netsvcs Accept, dev_key->ControllerIP %d, "
                            "dev_key->SwitchIp %d", __FUNCTION__, __LINE__, 
                            dev_key->controller_ip_addr, dev_key->switch_ip_addr);
            memcpy(&dkey, dev_key, sizeof(cc_ofdev_key_t));

            found = TRUE;            
            g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);

            cc_of_global.NET_SVCS[TCP].accept_conn(listenfd, dkey);
        }
    }
    if (!found)
        g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
   

}


void process_tcpfd_pollin_func(char *tname,
                               adpoll_fd_info_t *data_p,
                               adpoll_send_msg_htbl_info_t *unused_data UNUSED)
{
    char buf[MAXBUF]; /* Allocate buf to read data */
    ssize_t read_len = 0;
    cc_ofchannel_key_t *fd_chann_key;
    int tcp_sockfd = data_p->fd;
    cc_of_ret status = CC_OF_OK;
    cc_ofrw_key_t rwkey;
    cc_ofrw_info_t *rwinfo = NULL;
    cc_ofdev_info_t *devinfo = NULL;
    
    if (data_p == NULL) {
        CC_LOG_ERROR("%s(%d): received NULL data",
                     __FUNCTION__, __LINE__);
        return;
    }

    CC_LOG_DEBUG("%s(%d)[%s]: Someone wants to talk TCP to me at %d!",
                 __FUNCTION__, __LINE__, tname, tcp_sockfd);

    /* Read data from socket */
    if ((read_len = tcp_read(tcp_sockfd, buf, MAXBUF, 0, NULL, NULL)) < 0) {

        if (errno != EAGAIN) {
            CC_LOG_ERROR("%s(%d)[%s]: %s, Error while reading pkt on tcp sockfd: %d",
                         __FUNCTION__, __LINE__, tname,
                         strerror(errno), tcp_sockfd);
        } else {
            CC_LOG_DEBUG("%s(%d)[%s]: EWOULDBLOCK..!", __FUNCTION__,
                         __LINE__, tname);
        }
        return;
    }
    CC_LOG_DEBUG("%s(%d): RECEIVED PKT LENGTH IS %zd on channel dp_id-%d aux_id-%d",
                  __FUNCTION__, __LINE__, read_len, tcp_sockfd, tcp_sockfd);
 
    /* Dropping all TCP control pkts */
#if 0
    if (read_len == 0) {
        CC_LOG_DEBUG("%s(%d): Drop this pkt as this is a TCP controll" 
                     "pkt and not an OFP packet on channel dp_id-%d aux_id-%d",
                     __FUNCTION__, __LINE__, tcp_sockfd, tcp_sockfd);
        return;
    }
#endif

    CC_LOG_DEBUG("%s(%d)[%s]: Received a message %s",
                 __FUNCTION__, __LINE__, tname, buf);
    g_mutex_lock(&cc_of_global.ofdev_htbl_lock);
    g_mutex_lock(&cc_of_global.ofchannel_htbl_lock);
    g_mutex_lock(&cc_of_global.ofrw_htbl_lock);

    print_ofchann_htbl();
    status = find_ofchann_key_rwsocket(tcp_sockfd, &fd_chann_key);
    if (status < 0) {
        CC_LOG_ERROR("%s(%d)[%s]: could not find ofchann key for sockfd %d",

                     __FUNCTION__, __LINE__, tname, tcp_sockfd);
        g_mutex_unlock(&cc_of_global.ofrw_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofchannel_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
        return;
    }

    /* 
     * Do a reverse lookup to get the dev_key 
     * corresponding to this tcp sockfd
     */
    rwkey.rw_sockfd = tcp_sockfd;
    rwinfo = g_hash_table_lookup(cc_of_global.ofrw_htbl, &rwkey);
    if (rwinfo == NULL) {
        CC_LOG_ERROR("%s(%d)[%s]: could not find rwsockinfo in ofrw_htbl"
                     "for sockfd-%d", __FUNCTION__, __LINE__, tname,
                     rwkey.rw_sockfd);
        g_mutex_unlock(&cc_of_global.ofrw_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofchannel_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
        
        return;
    }

    if ((cc_of_global.ofdev_type == CONTROLLER) && 
        (rwinfo->state != CC_OF_RW_UP)){
        /* Drop this pkt as the controller is not ready to recv mesgs on this
         * channel yet. Let it finish initilaizing this channel info and 
         * then send pkts. 
         */
        CC_LOG_DEBUG("%s(%d): Drop this pkt as the controller is not"
                     "ready to rev mesgs on TCP channel dp_id-%lu aux_id-%u",
                     __FUNCTION__, __LINE__, tcp_sockfd, tcp_sockfd);
        g_mutex_unlock(&cc_of_global.ofrw_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofchannel_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
        
		return;
    }

    devinfo = g_hash_table_lookup(cc_of_global.ofdev_htbl, &(rwinfo->dev_key));
    if (devinfo == NULL) {
        CC_LOG_ERROR("%s(%d)[%s]: could not find devinfo in ofdev_htbl"
                     "for device", __FUNCTION__, __LINE__,tname);
        g_mutex_unlock(&cc_of_global.ofrw_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofchannel_htbl_lock);
        g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
        return;
    }


    /* Send data to controller/switch via their callback */
    devinfo->recv_func(fd_chann_key->dp_id, fd_chann_key->aux_id, 
                        buf, read_len);
    
    CC_LOG_DEBUG("%s(%d)[%s]: Read a pkt on tcp sockfd: %d, dp_id: %lu, aux_id: %u"
                "and sent it to controller/switch", __FUNCTION__, __LINE__,
                tname, tcp_sockfd, fd_chann_key->dp_id,
                fd_chann_key->aux_id);

    g_mutex_unlock(&cc_of_global.ofrw_htbl_lock);
    g_mutex_unlock(&cc_of_global.ofchannel_htbl_lock);
    g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
    
}


void process_tcpfd_pollout_func(char *tname,
                                adpoll_fd_info_t *data_p,
                                adpoll_send_msg_htbl_info_t *send_msg_p)
{
    int tcp_sockfd = 0;

    if (data_p == NULL) {
        CC_LOG_ERROR("%s(%d)[%s]: received NULL data",
                     __FUNCTION__, __LINE__, tname);
    }

    if (send_msg_p == NULL) {
        CC_LOG_ERROR("%s(%d)[%s]: send message invalid",
                     __FUNCTION__, __LINE__, tname);
    }

    tcp_sockfd = data_p->fd;

    /* Call tcpsocket send fn */
    if (tcp_write(tcp_sockfd, send_msg_p->data, send_msg_p->data_size, 0, NULL, 0) < 0) {
        CC_LOG_ERROR("%s(%d)[%s]: %s, error while sending pkt on tcp sockfd: %d", 
                     __FUNCTION__, __LINE__, tname, strerror(errno), tcp_sockfd);
        return;
    } 

    CC_LOG_DEBUG("%s(%d)[%s]: Sent a pkt out on tcp sockfd: %d", __FUNCTION__, 
                __LINE__, tname, tcp_sockfd);

}


cc_of_ret tcp_open_clientfd(cc_ofdev_key_t key, cc_ofchannel_key_t ofchann_key)
{
    int clientfd;
    cc_of_ret status = CC_OF_OK;
    int optval = 1;
    struct sockaddr_in serveraddr, localaddr;
    struct timeval timeout;      
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    int sockflags;


    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return clientfd;
    }

    // To prevent "Address already in use" error from bind
    if ((status = setsockopt(clientfd, SOL_SOCKET,SO_REUSEADDR, 
                             (const void *)&optval, sizeof(int))) < 0) {
        CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, 
                     strerror(errno));
        return status;
    }

    sockflags = fcntl(clientfd,F_GETFL,0);
    g_assert(sockflags != -1);
    fcntl(clientfd, F_SETFL, sockflags | O_NONBLOCK);

    memset(&localaddr, 0, sizeof(localaddr));
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = htonl(key.switch_ip_addr);
    localaddr.sin_port = 0;  
    
    // Bind clienfd to a local interface addr
    if ((status = bind(clientfd, (struct sockaddr *) &localaddr, 
                       sizeof(localaddr))) < 0) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return status;
    }
 
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(key.controller_ip_addr);
    serveraddr.sin_port = htons(key.controller_L4_port);

    // Establish connection with server
    if ((status = connect(clientfd, (struct sockaddr *) &serveraddr, 
                          sizeof(serveraddr))) < 0) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return status;
    }
    
    // Add clientfd to a thr_mgr and update it in ofrw, ofdev htbls
    adpoll_thr_msg_t thr_msg;

    thr_msg.fd = clientfd;
    thr_msg.fd_type = SOCKET;
    thr_msg.fd_action = ADD;
    thr_msg.poll_events = POLLIN | POLLOUT;
    thr_msg.pollin_func = &process_tcpfd_pollin_func;
    thr_msg.pollout_func = &process_tcpfd_pollout_func;
        
    status = cc_add_sockfd_rw_pollthr(&thr_msg, key, TCP, ofchann_key);
    if (status < 0) {
	    CC_LOG_ERROR("%s(%d):Error updating tcp sockfd in global structures: %s",
                     __FUNCTION__, __LINE__, cc_of_strerror(errno));
	    close(clientfd);
	    return status;
    }
    return clientfd;
}


cc_of_ret tcp_open_listenfd(cc_ofdev_key_t key)
{
    int listenfd;
    cc_of_ret status = CC_OF_OK;
    int optval = 1;
    struct sockaddr_in serveraddr;
    adpoll_thr_msg_t thr_msg;

    
    CC_LOG_DEBUG("%s(%d): Starting to open listen fd 0x%x:%d",
                 __FUNCTION__, __LINE__, key.controller_ip_addr,
                 key.controller_L4_port);
    
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    	CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return listenfd;
    }

    // To prevent "Address already in use" error from bind
    if ((status = setsockopt(listenfd, SOL_SOCKET,SO_REUSEADDR, 
                             (const void *)&optval, sizeof(int))) < 0) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return status;
    }

    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(key.controller_ip_addr);
    serveraddr.sin_port = htons(key.controller_L4_port);
    
    if ((status = bind(listenfd, (struct sockaddr *)&serveraddr, 
                                  sizeof(serveraddr))) < 0) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return status;
    }
    CC_LOG_DEBUG("%s(%d): listen fd established: %d",
                 __FUNCTION__, __LINE__, listenfd);

    if ((status = listen(listenfd, LISTENQ)) < 0) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return status;
    }

    // Add tcp_listenfd to the global oflisten_pollthr
    thr_msg.fd = listenfd;
    thr_msg.fd_type = SOCKET;
    thr_msg.fd_action = ADD;
    thr_msg.poll_events = POLLIN;
    thr_msg.pollin_func = &process_listenfd_pollin_func;
    thr_msg.pollout_func = NULL;

    status = adp_thr_mgr_add_del_fd(cc_of_global.oflisten_pollthr_p, &thr_msg);
    if (status < 0) {
        CC_LOG_ERROR("%s(%d):Error adding listenfd to oflisten_pollthr_p: %s",
                     __FUNCTION__, __LINE__, cc_of_strerror(errno));
        close(listenfd);
        return status;
    }

    return listenfd;
}


cc_of_ret tcp_accept(int listenfd, cc_ofdev_key_t key)
{
    int connfd;
    cc_of_ret status = CC_OF_OK;
    struct sockaddr_in clientaddr;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    adpoll_thr_msg_t thr_msg;
    cc_ofdev_info_t *dev_info = NULL;
    cc_ofchannel_key_t chann_key;
    cc_ofrw_key_t rw_key;
    cc_ofrw_info_t *rw_info = NULL;
    cc_ofrw_info_t rw_info_new;
    struct timeval timeout;
    int sockflags;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    gboolean new_entry;

    if ((connfd = accept(listenfd, (struct sockaddr *) &clientaddr, 
                         &addrlen)) < 0 ) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return connfd;
    }

    sockflags = fcntl(connfd,F_GETFL,0);
    g_assert(sockflags != -1);
    fcntl(connfd, F_SETFL, sockflags | O_NONBLOCK);

    #if 0
    if ((status = setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO,
                              (char *)&timeout, sizeof(timeout))) < 0) {
        CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, 
                     strerror(errno));
    }
    
    if ((status = setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO,
                             (char *)&timeout, sizeof(timeout))) < 0) {
        CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, 
                     strerror(errno));
    }
    #endif
    
    // Add connfd to a thr_mgr and update it in ofrw, ofdev htbls
    thr_msg.fd = connfd;
    thr_msg.fd_type = SOCKET;
    thr_msg.fd_action = ADD;
    thr_msg.poll_events = POLLIN | POLLOUT;
    thr_msg.pollin_func = &process_tcpfd_pollin_func;
    thr_msg.pollout_func = &process_tcpfd_pollout_func;
    
    status = cc_add_sockfd_rw_pollthr(&thr_msg, key, TCP, chann_key);
    if (status < 0) {
	    CC_LOG_ERROR("%s(%d):Error updating sockfd in global structures: %s",
                     __FUNCTION__, __LINE__, cc_of_strerror(errno));
	    close(connfd);
	    return status;
    }

    g_mutex_lock(&cc_of_global.ofdev_htbl_lock);    
    dev_info = g_hash_table_lookup(cc_of_global.ofdev_htbl, &key);

    if (dev_info == NULL) {
        CC_LOG_ERROR("%s(%d): could not find devinfo in ofdev_htbl"
                     "for device", __FUNCTION__, __LINE__);
        g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
        close(connfd);
        return CC_OF_EHTBL;
    }
 
    CC_LOG_DEBUG("%s(%d): client_ip:%d port:%d\n" , 
	    		 __FUNCTION__, __LINE__, 
				(uint32_t)clientaddr.sin_addr.s_addr,
				(uint16_t)(ntohs(clientaddr.sin_port)));
    /* Notify the controller about the new TCP channel */
    dev_info->accept_chann_func((uint64_t)connfd, (uint8_t)connfd, 
                                (uint32_t)(clientaddr.sin_addr.s_addr),
                                (uint16_t)(ntohs(clientaddr.sin_port)));

    g_mutex_unlock(&cc_of_global.ofdev_htbl_lock);
    

    /* Update the ofrw_state to CC_OF_RW_UP after controller is 
     * notified of this new channel 
     */
    g_mutex_lock(&cc_of_global.ofrw_htbl_lock);
    
    rw_key.rw_sockfd = connfd;
    rw_info = g_hash_table_lookup(cc_of_global.ofrw_htbl, &rw_key);
    if (rw_info == NULL) {
        CC_LOG_ERROR("%s(%d): could not find rwinfo in ofrw_htbl"
                     "for the newly connected socket %d", 
                     __FUNCTION__, __LINE__, connfd);
        return CC_OF_EHTBL;
    }
    memcpy(&rw_info_new, rw_info, sizeof(cc_ofrw_info_t));
    rw_info_new.state = CC_OF_RW_UP;
    update_global_htbl_lockfree(OFRW, ADD, (gpointer)&rw_key, (gpointer)&rw_info_new, 
                       &new_entry);
    CC_LOG_DEBUG("%s(%d): Updated TCP channel State to CC_OF_RW_UP",
                __FUNCTION__, __LINE__);
    print_ofrw_htbl();
    
    g_mutex_unlock(&cc_of_global.ofrw_htbl_lock);        

    return connfd;
}


ssize_t tcp_read(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr UNUSED, 
                 socklen_t *addrlen UNUSED)
{
    ssize_t ret_len;
    CC_LOG_DEBUG("%s(%d): Receiving from socket %d", __FUNCTION__, __LINE__, sockfd);
    ret_len = recv(sockfd, buf, len, flags);
    CC_LOG_DEBUG("%s(%d): Received %zd bytes from socket %d", __FUNCTION__, __LINE__, ret_len, sockfd);
    
    return ret_len;
} 


ssize_t tcp_write(int sockfd, const void *buf, size_t len, int flags,
                  const struct sockaddr *dest_addr UNUSED,
                  socklen_t addrlen UNUSED)
{
    return send(sockfd, buf, len, flags);
}


// caller should acquire three htbl locks
cc_of_ret tcp_close(int sockfd)
{
    cc_of_ret status = CC_OF_OK;
    adpoll_thread_mgr_t *tmgr = NULL;
    adpoll_thr_msg_t thr_msg;
    
    thr_msg.fd = sockfd;
    thr_msg.fd_type = SOCKET;
    thr_msg.fd_action = DELETE_FD;

    CC_LOG_DEBUG("%s(%d): Starting", __FUNCTION__, __LINE__);

#if 0
    status = find_ofchann_key_rwsocket(tcp_sockfd, &fd_chann_key);
    if (status < 0) {
        CC_LOG_ERROR("%s(%d): could not find ofchann key for sockfd %d",
                     __FUNCTION__, __LINE__, tcp_sockfd);
        return;
    }
    
    if (fd_chann_key->aux_id == 0) {
         /* If this is a main channel close all its auxiliary channels also */
        tcp_close();
    }
#endif

    status = find_thrmgr_rwsocket_lockfree(sockfd, &tmgr);
    
    if (status < 0) {
        CC_LOG_ERROR("%s(%d): could not find tmgr for tcp sockfd %d",
                     __FUNCTION__, __LINE__, sockfd);
        return status;
    }

    // Update global htbls
    status = cc_del_sockfd_rw_pollthr(tmgr, &thr_msg);
    if (status < 0) {
        CC_LOG_ERROR("%s(%d): %s, error while deleting tcp sockfd %d "
                     "from global structures", __FUNCTION__, __LINE__, 
                     cc_of_strerror(status), sockfd);
        return status;
    }

    if ((status = close(sockfd)) < 0) {
	    CC_LOG_ERROR("%s(%d): %s", __FUNCTION__, __LINE__, strerror(errno));
	    return status;
    }

    return status;
}
