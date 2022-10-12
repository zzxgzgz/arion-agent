//
// Created by ubuntu on 10/4/22.
//
#include <arpa/inet.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <assert.h>
#include <poll.h>
#include "af_xdp_user.h"
#include <sys/resource.h>
#include <signal.h>
#include <cstdlib>
#include <unistd.h>
#include <bpf.h>
#include <cerrno>
#include <sys/poll.h>
#include <csignal>
#include <cstring>

#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE      64
#define INVALID_UMEM_FRAME UINT64_MAX
#define MSG_DONTWAIT	= 0x40

/* VXLAN protocol (RFC 7348) header:
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |R|R|R|R|I|R|R|R|               Reserved                        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                VXLAN Network Identifier (VNI) |   Reserved    |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * I = VXLAN Network Identifier (VNI) present.
 */
struct vxlanhdr {
    __be32 vx_flags;
    __be32 vx_vni;
};

struct xsk_umem_info {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    void *buffer;
};

struct stats_record {
    uint64_t timestamp;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
};

struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;

    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t umem_frame_free;

    uint32_t outstanding_tx;

    struct stats_record stats;
    struct stats_record prev_stats;
};

static uint64_t xsk_alloc_umem_frame(struct xsk_socket_info *xsk)
{

    uint64_t frame;
    if (xsk->umem_frame_free == 0)
        return INVALID_UMEM_FRAME;

    frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
    xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;
    return frame;
}

static struct xsk_socket_info *xsk_configure_socket(struct config *cfg,
                                                    struct xsk_umem_info *umem)
{
    struct xsk_socket_config xsk_cfg;
    struct xsk_socket_info *xsk_info;
    uint32_t idx;
    uint32_t prog_id = 0;
    int i;
    int ret;

    xsk_info = static_cast<xsk_socket_info *>(calloc(1, sizeof(*xsk_info)));
    if (!xsk_info)
        return static_cast<xsk_socket_info *>(nullptr);

    xsk_info->umem = umem;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags = 0;
    xsk_cfg.xdp_flags = cfg->xdp_flags;
    xsk_cfg.bind_flags = cfg->xsk_bind_flags;
    ret = xsk_socket__create(&xsk_info->xsk, cfg->ifname,
                             cfg->xsk_if_queue, umem->umem, &xsk_info->rx,
                             &xsk_info->tx, &xsk_cfg);

    if (ret)
        goto error_exit;

    ret = bpf_get_link_xdp_id(cfg->ifindex, &prog_id, cfg->xdp_flags);
    if (ret)
        goto error_exit;

    /* Initialize umem frame allocation */

    for (i = 0; i < NUM_FRAMES; i++)
        xsk_info->umem_frame_addr[i] = i * FRAME_SIZE;

    xsk_info->umem_frame_free = NUM_FRAMES;

    /* Stuff the receive path with buffers, we assume we have enough */
    ret = xsk_ring_prod__reserve(&xsk_info->umem->fq,
                                 XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                 &idx);

    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS)
        goto error_exit;

    for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i ++)
        *xsk_ring_prod__fill_addr(&xsk_info->umem->fq, idx++) =
                xsk_alloc_umem_frame(xsk_info);

    xsk_ring_prod__submit(&xsk_info->umem->fq,
                          XSK_RING_PROD__DEFAULT_NUM_DESCS);

    return xsk_info;

error_exit:
    errno = -ret;
    return static_cast<xsk_socket_info *>(nullptr);
}

static const struct option_wrapper long_options[] = {

    {{"help",	 no_argument, nullptr, 'h' },
      "Show help", "",false},

    {{"dev",	 required_argument,	nullptr, 'd' },
      "Operate on device <ifname>", "<ifname>", true},

    {{"skb-mode",	 no_argument, nullptr, 'S' },
      "Install XDP program in SKB (AKA generic) mode"},

    {{"native-mode", no_argument, nullptr, 'N' },
      "Install XDP program in native mode"},

    {{"auto-mode",	 no_argument,		nullptr, 'A' },
      "Auto-detect SKB or native mode"},

    {{"force",	 no_argument,		nullptr, 'F' },
      "Force install, replacing existing program on interface"},

    {{"copy",        no_argument,		nullptr, 'c' },
      "Force copy mode"},

    {{"zero-copy",	 no_argument,		nullptr, 'z' },
      "Force zero-copy mode"},

    {{"queue",	 required_argument,	nullptr, 'Q' },
      "Configure interface receive queue for AF_XDP, default=0"},

    {{"poll-mode",	 no_argument,		nullptr, 'p' },
      "Use the poll() API waiting for packets to arrive"},

    {{"unload",      no_argument,		nullptr, 'U' },
      "Unload XDP program instead of loading"},

    {{"quiet",	 no_argument,		nullptr, 'q' },
      "Quiet mode (no output)"},

    {{"filename",    required_argument,	nullptr,  1  },
      "Load program from <file>", "<file>"},

    {{"progsec",	 required_argument,	nullptr,  2  },
      "Load program in <section> of the ELF file", "<section>"},

    {{0, 0, nullptr,  0 }, nullptr, "",false}
};

static bool global_exit;

static uint64_t xsk_umem_free_frames(struct xsk_socket_info *xsk)
{
    return xsk->umem_frame_free;
}

static void xsk_free_umem_frame(struct xsk_socket_info *xsk, uint64_t frame)
{
    assert(xsk->umem_frame_free < NUM_FRAMES);

    xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static void complete_tx(struct xsk_socket_info *xsk)
{
    unsigned int completed;
    uint32_t idx_cq;

    if (!xsk->outstanding_tx)
        return;

    sendto(xsk_socket__fd(xsk->xsk), NULL, 0, 0X40/*MSG_DONTWAIT*/, NULL, 0);


    /* Collect/free completed TX buffers */
    completed = xsk_ring_cons__peek(&xsk->umem->cq,
                                    XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                    &idx_cq);

    if (completed > 0) {
        for (int i = 0; i < completed; i++)
            xsk_free_umem_frame(xsk,
                                *xsk_ring_cons__comp_addr(&xsk->umem->cq,
                                                          idx_cq++));

        xsk_ring_cons__release(&xsk->umem->cq, completed);
        xsk->outstanding_tx -= completed < xsk->outstanding_tx ?
                                       completed : xsk->outstanding_tx;
    }
}

static inline __sum16 csum16_add(__sum16 csum, __be16 addend)
{
    uint16_t res = (uint16_t)csum;

    res += (__u16)addend;
    return (__sum16)(res + (res < (__u16)addend));
}

static inline __sum16 csum16_sub(__sum16 csum, __be16 addend)
{
    return csum16_add(csum, ~addend);
}

static inline void csum_replace2(__sum16 *sum, __be16 old, __be16 present)
{
    *sum = ~csum16_add(csum16_sub(~(*sum), old), present);
}

static bool process_packet(struct xsk_socket_info *xsk,
                           uint64_t addr, uint32_t len)
{
    uint8_t *pkt = static_cast<uint8_t *>(xsk_umem__get_data(xsk->umem->buffer, addr));

    /* Lesson#3: Write an IPv6 ICMP ECHO parser to send responses
	 *
	 * Some assumptions to make it easier:
	 * - No VLAN handling
	 * - Only if nexthdr is ICMP
	 * - Just return all data with MAC/IP swapped, and type set to
	 *   ICMPV6_ECHO_REPLY
	 * - Recalculate the icmp checksum */

    if (true) {
        /*
         * TODO: Parse packet here, get VNI, IP, MAC, lookup locally in DB, and replace neigbor host IP if found;
         * if NOT found, drop packet and remotely GET from Arion Master.
         * */
        int ret;
        uint32_t tx_idx = 0;
        uint8_t tmp_mac[ETH_ALEN];
        struct in6_addr tmp_ip;
        struct ethhdr *eth = (struct ethhdr *) pkt;
        struct iphdr *ip = (struct iphdr *) (eth + sizeof(*eth));
        struct udphdr *udp = (struct udphdr *) (ip + sizeof(*ip));
        // TODO: find a way to get vxlan header
        struct vxlanhdr* vxlan = (struct vxlanhdr *)(udp + sizeof(*udp));
        struct iphdr *inner_ip = (struct iphdr *)(vxlan + sizeof(*vxlan));
        printf("VNI: %ld, Inner src IP: %d, dest IP: %d", vxlan->vx_vni, inner_ip->saddr, inner_ip->daddr);
        struct ipv6hdr *ipv6 = (struct ipv6hdr *) (eth + 1);
        struct icmp6hdr *icmp = (struct icmp6hdr *) (ipv6 + 1);

        if (ntohs(eth->h_proto) != ETH_P_IPV6 ||
            len < (sizeof(*eth) + sizeof(*ipv6) + sizeof(*icmp)) ||
            ipv6->nexthdr != IPPROTO_ICMPV6 ||
            icmp->icmp6_type != ICMPV6_ECHO_REQUEST)
            return false;

        memcpy(tmp_mac, eth->h_dest, ETH_ALEN);
        memcpy(eth->h_dest, eth->h_source, ETH_ALEN);
        memcpy(eth->h_source, tmp_mac, ETH_ALEN);

        memcpy(&tmp_ip, &ipv6->saddr, sizeof(tmp_ip));
        memcpy(&ipv6->saddr, &ipv6->daddr, sizeof(tmp_ip));
        memcpy(&ipv6->daddr, &tmp_ip, sizeof(tmp_ip));

        icmp->icmp6_type = ICMPV6_ECHO_REPLY;

        csum_replace2(&icmp->icmp6_cksum,
                      htons(ICMPV6_ECHO_REQUEST << 8),
                      htons(ICMPV6_ECHO_REPLY << 8));

        /* Here we sent the packet out of the receive port. Note that
		 * we allocate one entry and schedule it. Your design would be
		 * faster if you do batch processing/transmission */

        ret = xsk_ring_prod__reserve(&xsk->tx, 1, &tx_idx);
        if (ret != 1) {
            /* No more transmit slots, drop the packet */
            return false;
        }

        xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->addr = addr;
        xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->len = len;
        xsk_ring_prod__submit(&xsk->tx, 1);
        xsk->outstanding_tx++;

        xsk->stats.tx_bytes += len;
        xsk->stats.tx_packets++;
        return true;
    }

    return false;
}


static void handle_receive_packets(struct xsk_socket_info *xsk)
{
    unsigned int rcvd, stock_frames, i;
    uint32_t idx_rx = 0, idx_fq = 0;
    int ret;

    rcvd = xsk_ring_cons__peek(&xsk->rx, RX_BATCH_SIZE, &idx_rx);
    if (!rcvd)
        return;

    /* Stuff the ring with as much frames as possible */
    stock_frames = xsk_prod_nb_free(&xsk->umem->fq,
                                    xsk_umem_free_frames(xsk));

    if (stock_frames > 0) {

        ret = xsk_ring_prod__reserve(&xsk->umem->fq, stock_frames,
                                     &idx_fq);

        /* This should not happen, but just in case */
        while (ret != stock_frames)
            ret = xsk_ring_prod__reserve(&xsk->umem->fq, rcvd,
                                         &idx_fq);

        for (i = 0; i < stock_frames; i++)
            *xsk_ring_prod__fill_addr(&xsk->umem->fq, idx_fq++) =
                    xsk_alloc_umem_frame(xsk);

        xsk_ring_prod__submit(&xsk->umem->fq, stock_frames);
    }

    /* Process received packets */
    for (i = 0; i < rcvd; i++) {
        uint64_t addr = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx)->addr;
        uint32_t len = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++)->len;

        if (!process_packet(xsk, addr, len))
            xsk_free_umem_frame(xsk, addr);

        xsk->stats.rx_bytes += len;
    }

    xsk_ring_cons__release(&xsk->rx, rcvd);
    xsk->stats.rx_packets += rcvd;

    /* Do we need to wake up the kernel for transmission */
    complete_tx(xsk);
}

static void rx_and_process(struct config *cfg,
                           struct xsk_socket_info *xsk_socket)
{
    struct pollfd fds[2];
    int ret, nfds = 1;

    memset(fds, 0, sizeof(fds));
    fds[0].fd = xsk_socket__fd(xsk_socket->xsk);
    fds[0].events = POLLIN;

    while(!global_exit) {
        if (cfg->xsk_poll_mode) {
            ret = poll(fds, nfds, -1);
            if (ret <= 0 || ret > 1)
                continue;
        }
        handle_receive_packets(xsk_socket);
    }
}

static void exit_application(int signal)
{
    signal = signal;
    global_exit = true;
}

static struct xsk_umem_info *configure_xsk_umem(void *buffer, uint64_t size)
{
    struct xsk_umem_info *umem;
    int ret;

    umem = static_cast<xsk_umem_info *>(calloc(1, sizeof(*umem)));
    if (!umem)
        return nullptr;

    ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq,
                           nullptr);
    if (ret) {
        errno = -ret;
        return nullptr;
    }

    umem->buffer = buffer;
    return umem;
}

void af_xdp_user::run_af_xdp(int argc, char *argv[])
{
    printf("%s", "af_xdp started");
    int ret;
    int xsks_map_fd;
    void *packet_buffer;
    uint64_t packet_buffer_size;
    struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
    struct config cfg;

    cfg.ifindex = -1;
    cfg.do_unload = false;
    // TODO: fill in the file name and progsec in CPP style
    struct xsk_umem_info *umem;
    struct xsk_socket_info *xsk_socket;
    struct bpf_object *bpf_obj = nullptr;

    /* Global shutdown handler*/
    signal(SIGINT, exit_application);

    /* Command line options can change progsec*/
    parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);

    /* Required option */
    if (cfg.ifindex == -1) {
        printf("%s", "ERROR: Required option --dev missing\n\n");
        usage(argv[0], __doc__, long_options, (argc == 1));
        exit(EXIT_FAIL_OPTION);
    }

    /* Unload XDP program if requested */
    if (cfg.do_unload) {
        int rc = xdp_link_detach(cfg.ifindex, cfg.xdp_flags, 0);
        exit(rc);
    }

    /* Load custom program if configured */
    if (cfg.filename[0] != 0) {
        struct bpf_map *map;

        bpf_obj = load_bpf_and_xdp_attach(&cfg);
        if (!bpf_obj) {
            /* Error handling done in load_bpf_and_xdp_attach() */
            exit(EXIT_FAILURE);
        }

        /* We also need to load the xsks_map */
        map = bpf_object__find_map_by_name(bpf_obj, "xsks_map");
        xsks_map_fd = bpf_map__fd(map);
        if (xsks_map_fd < 0) {
            fprintf(stderr, "ERROR: no xsks map found: %s\n",
                    strerror(xsks_map_fd));
            exit(EXIT_FAILURE);
        }
    }

    /* Allow unlimited locking of memory, so all memory needed for packet
	 * buffers can be locked.
	 */
    if (setrlimit(RLIMIT_MEMLOCK, &rlim)) {
        printf("%s", "ERROR: setrlimit(RLIMIT_MEMLOCK) \n");
        exit(EXIT_FAILURE);
    }

    /* Allocate memory for NUM_FRAMES of the default XDP frame size */
    packet_buffer_size = NUM_FRAMES * FRAME_SIZE;
    if (posix_memalign(&packet_buffer,
                       getpagesize(), /* PAGE_SIZE aligned */
                       packet_buffer_size)) {
        fprintf(stderr, "ERROR: Can't allocate buffer memory \"%s\"\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Initialize shared packet_buffer for umem usage */
    umem = configure_xsk_umem(packet_buffer, packet_buffer_size);
    if (umem == NULL) {
        fprintf(stderr, "ERROR: Can't create umem \"%s\"\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Open and configure the AF_XDP (xsk) socket */
    xsk_socket = xsk_configure_socket(&cfg, umem);
    if (xsk_socket == NULL) {
        fprintf(stderr, "ERROR: Can't setup AF_XDP socket \"%s\"\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Receive and count packets than drop them */
    rx_and_process(&cfg, xsk_socket);

    /* Cleanup */
    xsk_socket__delete(xsk_socket->xsk);
    xsk_umem__delete(umem->umem);
    xdp_link_detach(cfg.ifindex, cfg.xdp_flags, 0);

    return /*EXIT_OK*/;
}
