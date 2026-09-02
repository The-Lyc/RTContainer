#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rtems/score/netContainer.h>
#include <rtems/score/container.h>
#include <rtems/score/threadimpl.h>
#include <rtems/score/wkspace.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include <rtems/rtems/intr.h>
#include <rtems/rtems_bsdnet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <sys/socket.h>
#include <sys/domain.h>
#include <sys/sockio.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_pcb.h>

#include <rtems/score/containerlog.h>

#define UDBHASHSIZE 64
#define TCBHASHSIZE 128

extern struct ifnet *ifnet;
extern struct ifaddr **ifnet_addrs;

static int g_netContainerId = 1;
static int g_currentNetContainerNum = 0;

static void *net_hashinit_local(int count, u_long *hashmask)
{
    int n = 1;
    struct inpcbhead *table;

    while (n < count) {
        n <<= 1;
    }
    
    table = calloc((size_t)n, sizeof(*table));
    if (table == NULL) {
        return NULL;
    }

    *hashmask = (u_long)(n - 1);
    return table;
}

static void net_hashfree_local(void *p)
{
    if (p != NULL) {
        free(p);
    }
}

static net_group* net_group_new(void);
static void net_group_free(net_group *group);
static bool switch_to_root_net(Thread_Control *thread, void *arg);
static int create_loopback_for_container(NetContainer *netContainer);

static int install_container_loopback_route(NetContainer *netContainer)
{
    struct in_ifaddr *ia;

    if (netContainer == NULL || netContainer->group == NULL) {
        return -1;
    }

    for (ia = netContainer->group->in_ifaddr; ia != NULL; ia = ia->ia_next) {
        if (ia->ia_ifp != NULL && ia->ia_ifp->if_name != NULL &&
            strcmp(ia->ia_ifp->if_name, "lo") == 0) {
            break;
        }
    }

    if (ia == NULL) {
        return -1;
    }

    ia->ia_ifa.ifa_addr = (struct sockaddr *) &ia->ia_addr;
    ia->ia_ifa.ifa_dstaddr = (struct sockaddr *) &ia->ia_addr;
    ia->ia_ifa.ifa_netmask = (struct sockaddr *) &ia->ia_sockmask;
    ia->ia_ifa.ifa_ifp = ia->ia_ifp;

    if (ia->ia_flags & IFA_ROUTE) {
        (void) rtinit(&(ia->ia_ifa), RTM_DELETE, RTF_HOST);
        ia->ia_flags &= ~IFA_ROUTE;
    }

    if (rtinit(&(ia->ia_ifa), RTM_ADD, RTF_HOST | RTF_UP) != 0) {
        return -1;
    }

    ia->ia_flags |= IFA_ROUTE;
    return 0;
}

static int configure_loopback_ipv4_in_container(NetContainer *netContainer)
{
    Thread_Control *self;
    NetContainer *srcContainer;
    short flags;
    struct sockaddr_in address;
    struct sockaddr_in netmask;
    int rc = 0;

    if (netContainer == NULL) {
        return -1;
    }

    self = (Thread_Control *) _Thread_Get_executing();
    if (self == NULL || self->container == NULL || self->container->netContainer == NULL) {
        return 0;
    }

    srcContainer = self->container->netContainer;
    if (srcContainer != netContainer) {
        rtems_net_container_move_task(srcContainer, netContainer, self);
    }

    flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
    if (rtems_bsdnet_ifconfig("lo0", SIOCSIFFLAGS, &flags) < 0) {
        rc = -1;
        goto restore;
    }

    memset(&netmask, 0, sizeof(netmask));
    netmask.sin_len = sizeof(netmask);
    netmask.sin_family = AF_INET;
    netmask.sin_addr.s_addr = htonl(IN_CLASSA_NET);
    if (rtems_bsdnet_ifconfig("lo0", SIOCSIFNETMASK, &netmask) < 0) {
        rc = -1;
        goto restore;
    }

    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (rtems_bsdnet_ifconfig("lo0", SIOCSIFADDR, &address) < 0) {
        rc = -1;
        goto restore;
    }

    if (install_container_loopback_route(netContainer) != 0) {
        rc = -1;
        goto restore;
    }

restore:
    if (srcContainer != netContainer) {
        rtems_net_container_move_task(netContainer, srcContainer, self);
    }

    return rc;
}

// 初始化根网络容器
int rtems_net_container_initialize_root(NetContainer **netContainer)
{
    CONTAINER_LOG_TRACE("Initializing root NET container");
    if (netContainer == NULL)
    {
        CONTAINER_LOG_ERROR("NET container pointer is NULL");
        return -1;
    }

    *netContainer = (NetContainer *)malloc(sizeof(NetContainer));     // 可能存在的问题是用户初始化函数使用这个的话用户空间还没启动， 所以使用这个函数存在错误，之后调试出现问题换成malloc
    if (*netContainer == NULL)
    {
        CONTAINER_LOG_ERROR("Failed to allocate memory for root NET container");
        return -1;
    }

    (*netContainer)->rc = 3;
    (*netContainer)->group = net_group_new(); 
    (*netContainer)->containerID = 1;

    if ((*netContainer)->group == NULL) {
        free(*netContainer);
        *netContainer = NULL;
        CONTAINER_LOG_ERROR("Failed to initialize net_group for root NET container");
        return -1;
    }

    g_currentNetContainerNum++;
    CONTAINER_LOG_INFO("Root NET container initialized successfully: ID=%d", (*netContainer)->containerID);

    return 0;
}

// 创建独立的net_group
static net_group* net_group_new(void)
{
    net_group *group = (net_group *)malloc(sizeof(net_group));
    if (group == NULL) {
        return NULL;
    }

    memset(group, 0, sizeof(net_group));
    
    // 初始化接口相关
    group->ifnet_p = NULL;          
    group->ifnet_addrs = NULL;      
    group->if_index_counter = 0;    
    group->if_indexlim = 8;         
    group->ifnet_config = NULL;
    
    // 初始化 UDP PCB
    group->udp_pcblist = (struct inpcbhead *)malloc(sizeof(struct inpcbhead));
    if (group->udp_pcblist == NULL) {
        free(group);
        return NULL;
    }
    LIST_INIT(group->udp_pcblist);
    
    group->udp_pcbinfo = (struct inpcbinfo *)malloc(sizeof(struct inpcbinfo));
    if (group->udp_pcbinfo == NULL) {
        free(group->udp_pcblist);
        free(group);
        return NULL;
    }
    memset(group->udp_pcbinfo, 0, sizeof(struct inpcbinfo));
    group->udp_pcbinfo->listhead = group->udp_pcblist;
    group->udp_pcbinfo->hashbase = net_hashinit_local(UDBHASHSIZE, &group->udp_pcbinfo->hashmask);
    if (group->udp_pcbinfo->hashbase == NULL) {
        free(group->udp_pcbinfo);
        free(group->udp_pcblist);
        free(group);
        return NULL;
    }
    
    // 初始化 TCP PCB
    group->tcp_pcblist = (struct inpcbhead *)malloc(sizeof(struct inpcbhead));
    if (group->tcp_pcblist == NULL) {
        net_hashfree_local(group->udp_pcbinfo->hashbase);
        free(group->udp_pcbinfo);
        free(group->udp_pcblist);
        free(group);
        return NULL;
    }
    LIST_INIT(group->tcp_pcblist);
    
    group->tcp_pcbinfo = (struct inpcbinfo *)malloc(sizeof(struct inpcbinfo));
    if (group->tcp_pcbinfo == NULL) {
        free(group->tcp_pcblist);
        net_hashfree_local(group->udp_pcbinfo->hashbase);
        free(group->udp_pcbinfo);
        free(group->udp_pcblist);
        free(group);
        return NULL;
    }
    memset(group->tcp_pcbinfo, 0, sizeof(struct inpcbinfo));
    group->tcp_pcbinfo->listhead = group->tcp_pcblist;
    group->tcp_pcbinfo->hashbase = net_hashinit_local(TCBHASHSIZE, &group->tcp_pcbinfo->hashmask);
    if (group->tcp_pcbinfo->hashbase == NULL) {
        free(group->tcp_pcbinfo);
        free(group->tcp_pcblist);
        net_hashfree_local(group->udp_pcbinfo->hashbase);
        free(group->udp_pcbinfo);
        free(group->udp_pcblist);
        free(group);
        return NULL;
    }

#ifdef RTEMSCFG_Carousel_CONTAINER
    /* 默认启用Carousel流调度器，默认速率 10 Mbps */
    group->carousel = carousel_init(10 * 1000000);
    group->carousel_default_rate = 10 * 1000000;
    group->carousel_enabled = (group->carousel != NULL);
    
    if (group->carousel == NULL) {
        printf("Warning: Failed to initialize Carousel for net_group\n");
    }
#endif

    // 分配ifnet_addrs
    group->ifnet_addrs = (struct ifaddr **)malloc(
        group->if_indexlim * sizeof(struct ifaddr *)
    );
    if (group->ifnet_addrs == NULL) {
#ifdef RTEMSCFG_Carousel_CONTAINER
        if (group->carousel != NULL) {
            carousel_destroy(group->carousel);
        }
#endif
        net_hashfree_local(group->tcp_pcbinfo->hashbase);
        free(group->tcp_pcbinfo);
        free(group->tcp_pcblist);
        net_hashfree_local(group->udp_pcbinfo->hashbase);
        free(group->udp_pcbinfo);
        free(group->udp_pcblist);
        free(group);
        return NULL;
    }
    memset(group->ifnet_addrs, 0, group->if_indexlim * sizeof(struct ifaddr *));
    
    /* 初始化 IPv4 地址链表 */
    group->in_ifaddr = NULL;
    
    // 初始化路由表为NULL,将在第一次访问时延迟初始化 */
    for (int i = 0; i < 32; i++) {
        group->rt_tables[i] = NULL;
    }
    
    /* 注释掉自动创建veth,由应用程序手动创建veth对 */
    /* group->veth_sc = veth_create("veth0", group); */
    group->veth_sc = NULL;  /* 初始化为NULL */
    
    return group;
}
// 释放net_group
static void net_group_free(net_group *group)
{
    if (group == NULL) {
        return;
    }
    
#ifdef RTEMSCFG_Carousel_CONTAINER
    /* 清理Carousel调度器 */
    if (group->carousel != NULL) {
        carousel_destroy(group->carousel);
        group->carousel = NULL;
    }
#endif
    
    // 释放 TCP PCB
    if (group->tcp_pcbinfo) {
        if (group->tcp_pcbinfo->hashbase) {
            net_hashfree_local(group->tcp_pcbinfo->hashbase);
        }
        free(group->tcp_pcbinfo);
    }
    if (group->tcp_pcblist) {
        free(group->tcp_pcblist);
    }
    
    // 释放 UDP PCB
    if (group->udp_pcbinfo) {
        if (group->udp_pcbinfo->hashbase) {
            net_hashfree_local(group->udp_pcbinfo->hashbase);
        }
        free(group->udp_pcbinfo);
    }
    if (group->udp_pcblist) {
        free(group->udp_pcblist);
    }
    
    if (group->ifnet_addrs != NULL) {
        free(group->ifnet_addrs);
    }
    
    free(group);
}

// 为容器创建独立的 loopback 接口
static int create_loopback_for_container(NetContainer *netContainer)
{
    extern int rtems_bsdnet_initialize_loop_for_container(void *net_group_ptr);
    int rc;

    if (!netContainer || !netContainer->group) {
        CONTAINER_LOG_ERROR("Invalid NET container while creating loopback");
        return -1;
    }

    // 为容器创建独立的 loopback 接口，传递整个 NetGroup 结构
    rc = rtems_bsdnet_initialize_loop_for_container(netContainer->group);
    if (rc != 0) {
        printf("容器%d: loopback 初始化失败 rc=%d\n", netContainer->containerID, rc);
        CONTAINER_LOG_ERROR(
          "Failed to initialize loopback for NET container %d: rc=%d",
          netContainer->containerID,
          rc
        );
        return -1;
    }

    if (netContainer->group->ifnet_p == NULL || netContainer->group->in_ifaddr == NULL) {
        printf("容器%d: loopback 数据不完整 ifnet=%p in_ifaddr=%p\n",
               netContainer->containerID,
               (void *)netContainer->group->ifnet_p,
               (void *)netContainer->group->in_ifaddr);
        CONTAINER_LOG_ERROR(
          "Incomplete loopback data for NET container %d",
          netContainer->containerID
        );
        return -1;
    }

    /* Route installation can fail during early bring-up; keep container creation non-fatal. */
    (void) install_container_loopback_route(netContainer);

    /* Configure IPv4 only if current thread/container context is usable; failures are non-fatal. */
    (void) configure_loopback_ipv4_in_container(netContainer);

    // printf("容器%d: 创建独立的 loopback 接口\n", netContainer->containerID);
    CONTAINER_LOG_INFO("Loopback ready for NET container: ID=%d", netContainer->containerID);
    return 0;
}

// 创建子net容器
NetContainer *rtems_net_container_create(void)
{
    CONTAINER_LOG_TRACE("Creating new NET container");
    NetContainer *netContainer = (NetContainer *)malloc(sizeof(NetContainer));
    if (netContainer == NULL)
    {
        CONTAINER_LOG_ERROR("Failed to allocate memory for new NET container");
        return NULL;
    }

    netContainer->rc = 1; // 初始引用计数为1，避免首次 move_task 后被提前删除
    netContainer->containerID = ++g_netContainerId;
    
    // 创建net_group
    netContainer->group = net_group_new();
    if (netContainer->group == NULL) {
        free(netContainer);
        CONTAINER_LOG_ERROR("Failed to initialize net_group for NET container");
        return NULL;
    }

    // printf("创建网络隔离容器: ID=%d\n", netContainer->containerID);

    // 为容器创建独立的 loopback 接口
    if (create_loopback_for_container(netContainer) != 0) {
        printf("错误: 容器%d loopback 接口创建失败\n", netContainer->containerID);
        net_group_free(netContainer->group);
        free(netContainer);
        CONTAINER_LOG_ERROR("Failed to create loopback for NET container: ID=%d", netContainer->containerID);
        return NULL;
    }

    g_currentNetContainerNum++;
    rtems_net_container_add_to_list(netContainer);
    CONTAINER_LOG_INFO("New NET container created successfully: ID=%d", netContainer->containerID);

    return netContainer;
}

static bool switch_to_root_net(Thread_Control *thread, void *arg)
{
    NetContainer *netContainer = (NetContainer *)arg;
    Container *container = rtems_container_get_root();
    NetContainer *root = container->netContainer;

    if (thread->container && thread->container->netContainer == netContainer)
    {
        thread->container->netContainer = root;
        root->rc++;
        netContainer->rc--;
        printf("线程ID=0x%08" PRIx32 " 切换到根网络容器\n", thread->Object.id);
    }
    return true;
}
// 删除子net容器
void rtems_net_container_delete(NetContainer *netContainer)
{
    CONTAINER_LOG_TRACE(
      "Deleting NET container: container_id=%d",
      netContainer ? netContainer->containerID : -1
    );
    if (!netContainer)
        return;

    Container *container = rtems_container_get_root();
    if (!container || !container->netContainer)
        return;

    NetContainer *root = container->netContainer;
    if (netContainer == root)
        return;

    // printf("删除子net容器: ID=%d, rc=%d\n", netContainer->containerID, netContainer->rc);

    // 将所有使用该容器的任务切换到根容器
    rtems_task_iterate(switch_to_root_net, netContainer);

    // 检查当前正在执行的线程是否已转换成功
    Thread_Control *self = (Thread_Control *)_Thread_Get_executing();
    if (self && self->container && self->container->netContainer == netContainer)
    {
        self->container->netContainer = root;
        root->rc++;
        netContainer->rc--;
    }

    // 从链表中移除
    rtems_net_container_remove_from_list(netContainer);

    // 清理ifnet里面的内容
    if (netContainer->group) {
        // 
        struct ifnet *ifp = netContainer->group->ifnet_p;
        if (ifp) {
            // 关闭接口
            if (ifp->if_flags & IFF_UP) {
                ifp->if_flags &= ~IFF_UP;
            }
            
            ifp->if_addrlist = NULL;
            
            memset(&ifp->if_data, 0, sizeof(ifp->if_data));
            
            ifp->if_next = NULL;
        }
        netContainer->group->ifnet_p = NULL;
        
        // 清理ifnet_addrs
        if (netContainer->group->ifnet_addrs) {
            for (int i = 0; i < netContainer->group->if_indexlim; i++) {
                if (netContainer->group->ifnet_addrs[i] != NULL) {
                    netContainer->group->ifnet_addrs[i] = NULL;
                }
            }
        }

        netContainer->group->ifnet_config = NULL;
        netContainer->group->if_index_counter = 0;

        // 释放net_group
        net_group_free(netContainer->group);
        netContainer->group = NULL;
    }

    // 减少全局容器计数
    g_currentNetContainerNum--;
    
    // 释放容器本身
    free(netContainer);
    CONTAINER_LOG_INFO("NET container deleted successfully");
}

// 将net容器添加到链表中
void rtems_net_container_add_to_list(NetContainer *netContainer)
{
    if (!netContainer)
        return;

    Container *container = rtems_container_get_root();
    if (!container)
        return;

    NetContainerNode **head = (NetContainerNode **)&container->netContainerListHead;
    NetContainerNode *new_node = (NetContainerNode *)malloc(sizeof(NetContainerNode));
    if (!new_node)
        return;

    new_node->netContainer = netContainer;
    new_node->next = *head;
    *head = new_node;

    // printf("添加net容器到链表: ID=%d, 地址=%p\n", netContainer->containerID, (void *)netContainer);    //调试完成之后再注释掉
    CONTAINER_LOG_DEBUG("NET container added to list: ID=%d", netContainer->containerID);
}

// 从链表中移除net容器
void rtems_net_container_remove_from_list(NetContainer *netContainer)
{
    if (!netContainer)
        return;

    Container *container = rtems_container_get_root();
    if (!container)
        return;

    NetContainerNode **head = (NetContainerNode **)&container->netContainerListHead;
    NetContainerNode *current = *head;
    NetContainerNode *prev = NULL;

    while (current)
    {
        if (current->netContainer == netContainer)
        {
            if (prev)
            {
                prev->next = current->next;
            }
            else
            {
                *head = current->next;
            }

            free(current);
            // printf("从链表中移除net容器: ID=%d, 地址=%p\n", netContainer->containerID, (void *)netContainer);    //调试完成之后再注释掉
            CONTAINER_LOG_DEBUG("NET container removed from list: ID=%d", netContainer->containerID);
            return;
        }
        prev = current;
        current = current->next;
    }
}

// 移动任务到指定的net容器
void rtems_net_container_move_task(NetContainer *srcContainer, NetContainer *destContainer, Thread_Control *thread)
{
    if (!srcContainer || !destContainer || !thread) {
        CONTAINER_LOG_ERROR("Invalid parameters for NET task move");
        return;
    }

    if (thread->container &&
        (thread->container->netContainer == srcContainer))
    {
        thread->container->netContainer = destContainer;
        srcContainer->rc--;
        if (srcContainer->rc <= 0 && srcContainer->containerID != 1)
        {
            rtems_net_container_delete(srcContainer);
        }
        destContainer->rc++;
        CONTAINER_LOG_INFO(
          "Task moved successfully: thread_id=%" PRIu32 " from net=%d to net=%d",
          thread->Object.id,
          srcContainer->containerID,
          destContainer->containerID
        );
    }
    else
    {
        printf("线程ID=0x%08" PRIx32 " 不在源net容器中\n", thread->Object.id);
        CONTAINER_LOG_WARN(
          "Thread not in source NET container: thread_id=%" PRIu32 ", src_id=%d",
          thread->Object.id,
          srcContainer->containerID
        );
    }
}

// 获取容器ID
int rtems_net_container_get_id(NetContainer *netContainer)
{
    return netContainer ? netContainer->containerID : -1;
}

// 获取容器引用计数
int rtems_net_container_get_rc(NetContainer *netContainer)
{
    return netContainer ? netContainer->rc : 0;
}

int rtems_net_container_get_NetContainerNum(void)
{
    return g_currentNetContainerNum;
}

struct ifnet *rtems_net_container_get_ifnet(void)
{
    rtems_interrupt_level level;
    rtems_interrupt_disable(level);
    Thread_Control *executing = _Thread_Get_executing();
    rtems_interrupt_enable(level);
    if (executing && executing->container && executing->container->netContainer) {
        NetContainer *netContainer = executing->container->netContainer;
        if (netContainer->group) {
            return netContainer->group->ifnet_p;
        }
    }
    return ifnet;
}

struct ifaddr **rtems_net_container_get_ifnet_addrs(void)
{
    rtems_interrupt_level level;
    rtems_interrupt_disable(level);
    Thread_Control *executing = _Thread_Get_executing();
    rtems_interrupt_enable(level);
    if (executing && executing->container && executing->container->netContainer) {
        NetContainer *netContainer = executing->container->netContainer;
        if (netContainer->group) {
            return netContainer->group->ifnet_addrs;
        }
    }
    return ifnet_addrs;
}

#ifdef RTEMSCFG_Carousel_CONTAINER
/* ========== Carousel深度集成API实现 ========== */

/* 为网络容器启用Carousel流调度器 */
int rtems_net_container_enable_carousel(
  NetContainer *netContainer,
  uint64_t default_rate_bps
)
{
    if (netContainer == NULL || netContainer->group == NULL) {
        return -1;
    }
    
    net_group *group = netContainer->group;
    
    /* 如果已经启用，先禁用 */
    if (group->carousel != NULL) {
        printf("Carousel already enabled for container %d, recreating...\n", 
               netContainer->containerID);
        carousel_destroy(group->carousel);
        group->carousel = NULL;
    }
    
    /* 使用默认值或指定值 */
    uint64_t rate = (default_rate_bps > 0) ? default_rate_bps : (10 * 1000000);
    
    /* 初始化Carousel */
    group->carousel = carousel_init(rate);
    if (group->carousel == NULL) {
        printf("Failed to initialize Carousel for container %d\n", 
               netContainer->containerID);
        group->carousel_enabled = false;
        return -1;
    }
    
    group->carousel_default_rate = rate;
    group->carousel_enabled = true;
    
    printf("Carousel enabled for container %d (default rate: %llu bps)\n", 
           netContainer->containerID, (unsigned long long)rate);
    
    return 0;
}

/* 禁用容器的Carousel调度器 */
void rtems_net_container_disable_carousel(NetContainer *netContainer)
{
    if (netContainer == NULL || netContainer->group == NULL) {
        return;
    }
    
    net_group *group = netContainer->group;
    
    if (group->carousel != NULL) {
        printf("Disabling Carousel for container %d\n", netContainer->containerID);
        carousel_destroy(group->carousel);
        group->carousel = NULL;
        group->carousel_enabled = false;
    }
}

/* 检查容器是否启用了Carousel */
bool rtems_net_container_is_carousel_enabled(NetContainer *netContainer)
{
    if (netContainer == NULL || netContainer->group == NULL) {
        return false;
    }
    
    return netContainer->group->carousel_enabled && 
           (netContainer->group->carousel != NULL);
}

/* 为容器中的特定流设置速率 */
int rtems_net_container_set_flow_rate(
  NetContainer *netContainer,
  uint32_t src_ip,
  uint32_t dst_ip,
  uint16_t src_port,
  uint16_t dst_port,
  uint8_t protocol,
  uint64_t rate_bps
)
{
    if (netContainer == NULL || netContainer->group == NULL) {
        return -1;
    }
    
    if (!netContainer->group->carousel_enabled || 
        netContainer->group->carousel == NULL) {
        printf("Carousel not enabled for container %d\n", 
               netContainer->containerID);
        return -1;
    }
    
    return carousel_set_flow_rate(
        netContainer->group->carousel,
        src_ip, dst_ip, src_port, dst_port, protocol,
        rate_bps
    );
}

/* 获取容器的Carousel统计信息 */
int rtems_net_container_get_carousel_stats(
  NetContainer *netContainer,
  uint32_t *total_flows,
  uint64_t *total_packets_sent,
  uint64_t *total_packets_dropped
)
{
    if (netContainer == NULL || netContainer->group == NULL) {
        return -1;
    }
    
    if (!netContainer->group->carousel_enabled || 
        netContainer->group->carousel == NULL) {
        return -1;
    }
    
    carousel_get_stats(
        netContainer->group->carousel,
        total_flows,
        total_packets_sent,
        total_packets_dropped
    );
    
    return 0;
}

#endif /* RTEMSCFG_Carousel_CONTAINER */

/* 获取当前容器的 IPv4 地址链表 */
struct in_ifaddr **rtems_net_container_get_in_ifaddr(void)
{
    Thread_Control *executing = _Thread_Get_executing();
    
    /* 如果当前线程没有容器上下文,返回全局地址链表 */
    if (executing == NULL || 
        executing->container == NULL || 
        executing->container->netContainer == NULL ||
        executing->container->netContainer->group == NULL) {
        /* 返回全局地址链表的地址 */
        extern struct in_ifaddr *in_ifaddr;
        return &in_ifaddr;
    }
    
    /* 返回容器的地址链表 */
    return &(executing->container->netContainer->group->in_ifaddr);
}

/* 获取当前容器的路由表 */
struct radix_node_head **rtems_net_container_get_rt_tables(void)
{
    Thread_Control *executing = _Thread_Get_executing();
    
    // 如果当前线程没有容器上下文,返回全局路由表
    if (executing == NULL || 
        executing->container == NULL || 
        executing->container->netContainer == NULL ||
        executing->container->netContainer->group == NULL) {

        extern struct radix_node_head *rt_tables[];
        return rt_tables;
    }
    
    net_group *group = executing->container->netContainer->group;
    

    if (group->rt_tables[2] == NULL) {
        extern struct domain *domains;
        struct domain *dom;

        for (dom = domains; dom; dom = dom->dom_next) {
            if (dom->dom_rtattach && group->rt_tables[dom->dom_family] == NULL) {
                dom->dom_rtattach((void *)&group->rt_tables[dom->dom_family],
                                  dom->dom_rtoffset);
            }
        }
    }
    
    return group->rt_tables;
}

/* 获取当前容器的 UDP PCB 链表 */
struct inpcbhead *rtems_net_container_get_udp_pcblist(void)
{
    Thread_Control *executing = _Thread_Get_executing();
    
    // 如果当前线程没有容器上下文,返回全局 PCB 链表
    if (executing == NULL || 
        executing->container == NULL || 
        executing->container->netContainer == NULL ||
        executing->container->netContainer->group == NULL) {
        extern struct inpcbhead udb;
        return &udb;
    }
    
    return executing->container->netContainer->group->udp_pcblist;
}

/* 获取当前容器的 UDP PCB info */
struct inpcbinfo *rtems_net_container_get_udp_pcbinfo(void)
{
    Thread_Control *executing = _Thread_Get_executing();
    
    // 如果当前线程没有容器上下文,返回全局 PCB info
    if (executing == NULL || 
        executing->container == NULL || 
        executing->container->netContainer == NULL ||
        executing->container->netContainer->group == NULL) {
        extern struct inpcbinfo udbinfo;
        return &udbinfo;
    }
    
    return executing->container->netContainer->group->udp_pcbinfo;
}
