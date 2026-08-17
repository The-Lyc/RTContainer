/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Network container integration test.
 *
 * This program combines the interface, loopback, and port-isolation tests
 * which were previously run as three independent RTEMS applications.  The
 * network stack and RTEMS application configuration are intentionally kept
 * here only once.
 */

#ifdef rtems_object_id_get_api
#undef rtems_object_id_get_api
#endif
#ifdef rtems_object_id_get_class
#undef rtems_object_id_get_class
#endif
#ifdef rtems_object_id_get_index
#undef rtems_object_id_get_index
#endif
#ifdef rtems_object_id_get_node
#undef rtems_object_id_get_node
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rtems.h>
#include <rtems/rtems_bsdnet.h>

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_var.h>

#ifndef _KERNEL
#define _KERNEL
#define NET_ALL_UNDEF_KERNEL
#endif
#include <net/if_arp.h>
#ifdef NET_ALL_UNDEF_KERNEL
#undef _KERNEL
#undef NET_ALL_UNDEF_KERNEL
#endif

#include <netinet/if_ether.h>
#include <netinet/in.h>

#include <rtems/score/container.h>
#include <rtems/score/netContainer.h>
#include <rtems/score/threadimpl.h>
#include <rtems/score/wkspace.h>

#include <tmacros.h>
#include <rtems/rtems_bsdnet_internal.h>

/* These legacy BSD networking declarations are hidden behind _KERNEL. */
extern void m_freem(struct mbuf *m);
extern int ether_ioctl(struct ifnet *ifp, ioctl_command_t command, caddr_t data);
extern void if_attach_to_container(struct ifnet *ifp, struct net_group *group);
extern void ether_ifattach(struct ifnet *ifp);

const char rtems_test_name[] = "NET CONTAINER ALL";

#define NET_ALL_IF_STACK_SIZE      (RTEMS_MINIMUM_STACK_SIZE * 4)
#define NET_ALL_SOCKET_STACK_SIZE  (RTEMS_MINIMUM_STACK_SIZE * 4)
#define NET_ALL_LOOPBACK_PORT      8003
#define NET_ALL_SHARED_PORT        8004
#define NET_ALL_LOOPBACK_MESSAGE   "Hello"

/*
 * rtems_net_container_delete() currently frees data which is still referenced
 * by the legacy networking stack.  The standalone tests delete a container
 * only immediately before exiting, so the resulting heap damage is hidden.
 * This combined test deliberately keeps its five short-lived containers until
 * rtems_test_exit(); sockets, tasks, and semaphores are still released.
 */

static int net_all_move_task(rtems_id task_id, NetContainer *destination)
{
    ISR_lock_Context lock_context;
    Thread_Control *thread;
    NetContainer *source;

    thread = _Thread_Get(task_id, &lock_context);
    if (thread == NULL) {
        return -1;
    }

    if (thread->container == NULL || thread->container->netContainer == NULL) {
        _ISR_lock_ISR_enable(&lock_context);
        return -1;
    }

    source = thread->container->netContainer;
    _ISR_lock_ISR_enable(&lock_context);

    rtems_net_container_move_task(source, destination, thread);
    return thread->container->netContainer == destination ? 0 : -1;
}

static int net_all_create_binary(char a, char b, char c, char d, rtems_id *id)
{
    rtems_status_code sc;

    sc = rtems_semaphore_create(
        rtems_build_name(a, b, c, d),
        0,
        RTEMS_SIMPLE_BINARY_SEMAPHORE | RTEMS_PRIORITY,
        0,
        id
    );
    return sc == RTEMS_SUCCESSFUL ? 0 : -1;
}

static void net_all_delete_semaphore(rtems_id *id)
{
    if (*id != 0) {
        (void) rtems_semaphore_delete(*id);
        *id = 0;
    }
}

static void net_all_delete_task(rtems_id *id)
{
    if (*id != 0) {
        (void) rtems_task_delete(*id);
        *id = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Interface isolation test (formerly net_if.c).                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    struct arpcom arpcom;
    int unit;
    int container_id;
    uint8_t mac_address[6];
} net_all_if_softc;

static NetContainer *net_all_if_container[2];
static rtems_id net_all_if_task_id[2];
static rtems_id net_all_if_first_ready;
static rtems_id net_all_if_second_ready;
static rtems_id net_all_if_done[2];
static volatile int net_all_if_configured[2];
static volatile int net_all_if_isolated[2];
static struct rtems_bsdnet_ifconfig net_all_if_config[2];

static int net_all_if_output(
    struct ifnet *ifp,
    struct mbuf *m,
    struct sockaddr *destination,
    struct rtentry *route
)
{
    (void) destination;
    (void) route;

    ++ifp->if_opackets;
    if (m != NULL) {
        ifp->if_obytes += m->m_pkthdr.len;
        m_freem(m);
    }
    return 0;
}

static void net_all_if_init(void *arg)
{
    struct ifnet *ifp = arg;
    net_all_if_softc *softc = ifp->if_softc;

    printf(
        "[容器%d] 初始化网络接口 %s%d\n",
        softc->container_id,
        ifp->if_name,
        ifp->if_unit
    );
    ifp->if_flags |= IFF_UP | IFF_RUNNING;
}

static int net_all_if_ioctl(
    struct ifnet *ifp,
    ioctl_command_t command,
    caddr_t data
)
{
    switch (command) {
    case SIOCSIFFLAGS:
        if ((ifp->if_flags & IFF_UP) != 0) {
            if ((ifp->if_flags & IFF_RUNNING) == 0) {
                net_all_if_init(ifp);
            }
        } else {
            ifp->if_flags &= ~IFF_RUNNING;
        }
        return 0;
    case SIOCSIFADDR:
    case SIOCGIFADDR:
    case SIOCSIFNETMASK:
    case SIOCSIFBRDADDR:
    case SIOCSIFDSTADDR:
        return ether_ioctl(ifp, command, data);
    default:
        return EINVAL;
    }
}

static int net_all_if_attach(
    struct rtems_bsdnet_ifconfig *config,
    int attaching
)
{
    Thread_Control *executing;
    NetContainer *container;
    net_all_if_softc *softc;
    struct ifnet *ifp;
    const char *unit_text;
    int unit;

    if (!attaching) {
        return 1;
    }

    printf("附加容器网络驱动: %s\n", config->name);

    executing = _Thread_Get_executing();
    if (executing == NULL || executing->container == NULL ||
        executing->container->netContainer == NULL) {
        return 0;
    }
    container = executing->container->netContainer;

    unit_text = config->name;
    while (*unit_text != '\0' && !isdigit((unsigned char) *unit_text)) {
        ++unit_text;
    }
    unit = *unit_text == '\0' ? 0 : atoi(unit_text);

    softc = _Workspace_Allocate(sizeof(*softc));
    if (softc == NULL) {
        return 0;
    }
    memset(softc, 0, sizeof(*softc));

    softc->unit = unit;
    softc->container_id = container->containerID;
    softc->mac_address[0] = 0x02;
    softc->mac_address[1] = 0x00;
    softc->mac_address[2] = 0x5e;
    softc->mac_address[3] = 0x00;
    softc->mac_address[4] = (uint8_t) container->containerID;
    softc->mac_address[5] = (uint8_t) unit;

    ifp = &softc->arpcom.ac_if;
    ifp->if_softc = softc;
    ifp->if_unit = unit;
    ifp->if_name = "veth";
    ifp->if_mtu = ETHERMTU;
    ifp->if_init = net_all_if_init;
    ifp->if_ioctl = net_all_if_ioctl;
    ifp->if_output = net_all_if_output;
    ifp->if_flags = IFF_BROADCAST | IFF_MULTICAST;
    ifp->if_snd.ifq_maxlen = IFQ_MAXLEN;
    memcpy(softc->arpcom.ac_enaddr, softc->mac_address, ETHER_ADDR_LEN);

    if_attach_to_container(ifp, container->group);
    ether_ifattach(ifp);

    printf(
        "[容器%d] 成功创建虚拟网络接口 %s%d\n",
        container->containerID,
        ifp->if_name,
        ifp->if_unit
    );
    return 1;
}

static struct ifnet *net_all_find_interface(
    NetContainer *container,
    const char *name,
    int unit
)
{
    struct ifnet *ifp;

    if (container == NULL || container->group == NULL) {
        return NULL;
    }

    for (ifp = container->group->ifnet_p; ifp != NULL; ifp = ifp->if_next) {
        if (ifp->if_name != NULL && strcmp(ifp->if_name, name) == 0 &&
            ifp->if_unit == unit) {
            return ifp;
        }
    }
    return NULL;
}

static int net_all_list_interfaces(NetContainer *container)
{
    struct ifnet *ifp;
    int count = 0;

    if (container == NULL || container->group == NULL) {
        printf("容器无效\n");
        return 0;
    }

    printf(
        "  容器%d(ID=%d, rc=%d)中的网络接口 (链表头地址=%p):\n",
        container->containerID,
        container->containerID,
        container->rc,
        (void *) container->group->ifnet_p
    );
    for (ifp = container->group->ifnet_p; ifp != NULL; ifp = ifp->if_next) {
        printf(
            "    - %s%d (索引=%d, 标志=0x%x, if_next=%p)\n",
            ifp->if_name,
            ifp->if_unit,
            ifp->if_index,
            ifp->if_flags,
            (void *) ifp->if_next
        );
        if (++count > 10) {
            printf("    警告: 接口链表过长，可能存在循环\n");
            break;
        }
    }
    if (count == 0) {
        printf("    (无接口)\n");
    }
    return count;
}

static int net_all_configure_interface(int index)
{
    static const char *const names[] = { "veth0", "veth1" };
    static const char *const addresses[] = { "192.168.1.10", "192.168.2.20" };
    Thread_Control *executing;
    NetContainer *container;
    struct sockaddr_in address;
    struct sockaddr_in netmask;
    short flags;

    memset(&net_all_if_config[index], 0, sizeof(net_all_if_config[index]));
    net_all_if_config[index].name = (char *) names[index];
    net_all_if_config[index].attach = net_all_if_attach;
    net_all_if_config[index].mtu = ETHERMTU;
    rtems_bsdnet_attach(&net_all_if_config[index]);

    executing = _Thread_Get_executing();
    container = executing->container->netContainer;
    if (net_all_find_interface(container, "veth", index) == NULL) {
        printf("[FAIL] 网络接口%s创建失败\n", names[index]);
        return -1;
    }
    printf("[PASS] 网络接口%s创建成功\n", names[index]);

    flags = IFF_UP;
    if (rtems_bsdnet_ifconfig(names[index], SIOCSIFFLAGS, &flags) != 0) {
        printf("[WARN] 启动接口失败: %s\n", strerror(errno));
        return -1;
    }
    printf("[PASS] 接口已启动(UP)\n");

    memset(&netmask, 0, sizeof(netmask));
    netmask.sin_len = sizeof(netmask);
    netmask.sin_family = AF_INET;
    netmask.sin_addr.s_addr = inet_addr("255.255.255.0");
    if (rtems_bsdnet_ifconfig(names[index], SIOCSIFNETMASK, &netmask) != 0) {
        printf("[WARN] 网络掩码配置失败: %s\n", strerror(errno));
        return -1;
    }
    printf("[PASS] 网络掩码配置成功\n");

    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(addresses[index]);
    if (index == 1) {
        printf("[调试] 准备配置IP: %s\n", addresses[index]);
        printf(
            "[调试] sockaddr_in: len=%d, family=%d, addr=0x%08x\n",
            address.sin_len,
            address.sin_family,
            (unsigned) ntohl(address.sin_addr.s_addr)
        );
    }
    if (rtems_bsdnet_ifconfig(names[index], SIOCSIFADDR, &address) != 0) {
        printf(
            "[WARN] IP地址配置失败: %s (errno=%d)\n",
            strerror(errno),
            errno
        );
        return -1;
    }
    printf("[PASS] IP地址配置成功: %s\n", addresses[index]);

    return 0;
}

static rtems_task net_all_if_task(rtems_task_argument argument)
{
    int index = (int) argument;
    int peer = 1 - index;
    Thread_Control *executing = _Thread_Get_executing();
    NetContainer *container = executing->container->netContainer;

    printf("\n=== 容器%d接口隔离测试开始 ===\n", index + 2);
    printf("当前线程容器ID: %d, rc=%d\n", container->containerID, container->rc);
    printf(
        "\n--- 测试%d: 创建容器%d网络接口 ---\n",
        index == 0 ? 1 : 3,
        index + 2
    );
    net_all_if_configured[index] = net_all_configure_interface(index) == 0;

    if (index == 0) {
        (void) rtems_semaphore_release(net_all_if_first_ready);
        (void) rtems_semaphore_obtain(
            net_all_if_second_ready,
            RTEMS_WAIT,
            RTEMS_NO_TIMEOUT
        );
    } else {
        (void) rtems_semaphore_obtain(
            net_all_if_first_ready,
            RTEMS_WAIT,
            RTEMS_NO_TIMEOUT
        );
        (void) rtems_semaphore_release(net_all_if_second_ready);
    }

    printf("\n--- %s ---\n", index == 0 ? "测试2: 验证接口隔离" : "列出容器3接口");
    (void) net_all_list_interfaces(container);
    net_all_if_isolated[index] =
        net_all_find_interface(container, "veth", index) != NULL &&
        net_all_find_interface(container, "veth", peer) == NULL;
    if (net_all_if_isolated[index]) {
        printf(
            "[PASS] 正确: 容器%d无法访问容器%d的veth%d接口\n",
            index + 2,
            peer + 2,
            peer
        );
    } else {
        printf(
            "[FAIL] 错误: 容器%d意外访问到了容器%d的接口\n",
            index + 2,
            peer + 2
        );
    }

    printf("\n=== 容器%d测试完成 ===\n", index + 2);

    (void) rtems_semaphore_release(net_all_if_done[index]);
    (void) rtems_task_suspend(RTEMS_SELF);
}

static int net_all_run_interface_test(void)
{
    rtems_status_code sc;
    int result = -1;
    int passed;
    int i;

    printf("\n[3/3] 网络接口隔离测试\n");
    printf("开始网络容器接口隔离测试\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("BSD网络栈已由 net_all 统一初始化\n");
    memset((void *) net_all_if_configured, 0, sizeof(net_all_if_configured));
    memset((void *) net_all_if_isolated, 0, sizeof(net_all_if_isolated));
    memset(net_all_if_container, 0, sizeof(net_all_if_container));
    memset(net_all_if_task_id, 0, sizeof(net_all_if_task_id));
    memset(net_all_if_done, 0, sizeof(net_all_if_done));
    net_all_if_first_ready = 0;
    net_all_if_second_ready = 0;

    if (net_all_create_binary('I', 'F', 'R', '1', &net_all_if_first_ready) != 0 ||
        net_all_create_binary('I', 'F', 'R', '2', &net_all_if_second_ready) != 0 ||
        net_all_create_binary('I', 'F', 'D', '1', &net_all_if_done[0]) != 0 ||
        net_all_create_binary('I', 'F', 'D', '2', &net_all_if_done[1]) != 0) {
        printf("  [FAIL] 创建接口测试信号量失败\n");
        goto cleanup;
    }

    printf("\n创建网络容器...\n");
    net_all_if_container[0] = rtems_net_container_create();
    net_all_if_container[1] = rtems_net_container_create();
    if (net_all_if_container[0] == NULL || net_all_if_container[1] == NULL) {
        printf("  [FAIL] 创建接口测试容器失败\n");
        goto cleanup;
    }
    printf(
        "[PASS] 网络容器创建成功: 容器2(ID=%d, rc=%d), 容器3(ID=%d, rc=%d)\n",
        net_all_if_container[0]->containerID,
        net_all_if_container[0]->rc,
        net_all_if_container[1]->containerID,
        net_all_if_container[1]->rc
    );

    for (i = 0; i < 2; ++i) {
        sc = rtems_task_create(
            rtems_build_name('I', 'F', 'T', (char) ('1' + i)),
            2,
            NET_ALL_IF_STACK_SIZE,
            RTEMS_DEFAULT_MODES,
            RTEMS_DEFAULT_ATTRIBUTES,
            &net_all_if_task_id[i]
        );
        if (sc != RTEMS_SUCCESSFUL ||
            net_all_move_task(net_all_if_task_id[i], net_all_if_container[i]) != 0) {
            printf("  [FAIL] 创建或迁移接口测试任务%d失败\n", i + 1);
            goto cleanup;
        }
        printf(
            "[PASS] 任务%d已分配到容器%d (rc=%d)\n",
            i + 1,
            net_all_if_container[i]->containerID,
            net_all_if_container[i]->rc
        );
    }

    printf("\n启动接口隔离测试任务...\n");
    printf("测试内容:\n");
    printf("  1. 容器2创建独立网络接口\n");
    printf("  2. 容器3创建独立网络接口\n");
    printf("  3. 验证容器间接口相互不可见\n");
    for (i = 0; i < 2; ++i) {
        sc = rtems_task_start(net_all_if_task_id[i], net_all_if_task, i);
        if (sc != RTEMS_SUCCESSFUL) {
            printf("  [FAIL] 启动接口测试任务%d失败\n", i + 1);
            goto cleanup;
        }
    }

    printf("等待测试完成...\n");
    (void) rtems_semaphore_obtain(net_all_if_done[0], RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    (void) rtems_semaphore_obtain(net_all_if_done[1], RTEMS_WAIT, RTEMS_NO_TIMEOUT);

    result = net_all_if_configured[0] && net_all_if_configured[1] &&
        net_all_if_isolated[0] && net_all_if_isolated[1] ? 0 : -1;
    passed = net_all_if_configured[0] +
        (net_all_if_isolated[0] && net_all_if_isolated[1]) +
        net_all_if_configured[1];
    printf("\n网络容器接口隔离测试结果\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("测试1 - 容器2网络接口创建    : %s\n", net_all_if_configured[0] ? "[PASS]" : "[FAIL]");
    printf("测试2 - 接口隔离验证         : %s\n", net_all_if_isolated[0] && net_all_if_isolated[1] ? "[PASS]" : "[FAIL]");
    printf("测试3 - 容器3网络接口创建    : %s\n", net_all_if_configured[1] ? "[PASS]" : "[FAIL]");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("总计: %d/3 测试通过 (%d%%)\n", passed, passed * 100 / 3);
    printf(
        "%s\n",
        result == 0 ?
            "所有测试通过! 网络容器接口隔离功能验证成功!" :
            "部分测试失败，请检查相关实现"
    );

cleanup:
    for (i = 0; i < 2; ++i) {
        net_all_delete_task(&net_all_if_task_id[i]);
    }
    net_all_delete_semaphore(&net_all_if_first_ready);
    net_all_delete_semaphore(&net_all_if_second_ready);
    net_all_delete_semaphore(&net_all_if_done[0]);
    net_all_delete_semaphore(&net_all_if_done[1]);
    printf("[PASS] 任务和同步信号量清理完成\n");
    printf("网络容器接口隔离测试结束\n");
    return result;
}

/* ------------------------------------------------------------------------- */
/* UDP loopback test (formerly net_loopback.c).                              */
/* ------------------------------------------------------------------------- */

static NetContainer *net_all_loop_container;
static rtems_id net_all_loop_task_id;
static rtems_id net_all_loop_done;
static volatile int net_all_loop_result;

static void net_all_print_loop_container_info(const char *stage)
{
    Thread_Control *executing = _Thread_Get_executing();

    printf("\n[%s] 容器信息:\n", stage);
    printf("  Thread: %p\n", (void *) executing);
    printf("  Container: %p\n", (void *) executing->container);
    if (executing->container != NULL) {
        NetContainer *container = executing->container->netContainer;

        printf("  NetContainer: %p\n", (void *) container);
        if (container != NULL) {
            printf("  Container ID: %d, rc=%d\n", container->containerID, container->rc);
            printf("  net_group: %p\n", (void *) container->group);
        }
    }
}

static rtems_task net_all_loop_task(rtems_task_argument argument)
{
    struct sockaddr_in address;
    struct timeval timeout = { 2, 0 };
    char buffer[32];
    ssize_t size;
    int socket_fd = -1;

    (void) argument;
    net_all_loop_result = -1;

    net_all_print_loop_container_info("任务启动");

    printf("\n[步骤1] 创建 socket\n");
    errno = 0;
    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    printf(
        "  socket() = %d (FD=%d, errno=%d: %s)\n",
        socket_fd,
        socket_fd,
        errno,
        strerror(errno)
    );
    if (socket_fd < 0) {
        net_all_loop_result = 1;
        goto done;
    }

    printf("\n[步骤2] 绑定到 127.0.0.1:%d\n", NET_ALL_LOOPBACK_PORT);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(NET_ALL_LOOPBACK_PORT);
    errno = 0;
    int bind_result = bind(socket_fd, (struct sockaddr *) &address, sizeof(address));
    printf("  bind() = %d\n", bind_result);
    if (bind_result != 0) {
        printf("  错误: %s\n", strerror(errno));
        net_all_loop_result = 2;
        goto done;
    }

    printf("\n[步骤3] 发送数据\n");
    size = sendto(
        socket_fd,
        NET_ALL_LOOPBACK_MESSAGE,
        sizeof(NET_ALL_LOOPBACK_MESSAGE),
        0,
        (struct sockaddr *) &address,
        sizeof(address)
    );
    if (size != (ssize_t) sizeof(NET_ALL_LOOPBACK_MESSAGE)) {
        printf("  sendto() 失败: %s\n", strerror(errno));
        net_all_loop_result = 3;
        goto done;
    }
    printf("  发送 %zd 字节: \"%s\"\n", size, NET_ALL_LOOPBACK_MESSAGE);

    rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 2);

    printf("\n[步骤4] 接收数据\n");
    (void) setsockopt(
        socket_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout)
    );
    memset(buffer, 0, sizeof(buffer));
    size = recvfrom(socket_fd, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
    if (size < 0) {
        printf("  recvfrom() 失败: %s\n", strerror(errno));
        net_all_loop_result = 4;
        goto done;
    }
    printf("  接收 %zd 字节: \"%s\"\n", size, buffer);

    printf("\n[步骤5] 验证数据\n");
    if (size == (ssize_t) sizeof(NET_ALL_LOOPBACK_MESSAGE) &&
        strcmp(buffer, NET_ALL_LOOPBACK_MESSAGE) == 0) {
        printf("  数据匹配!\n");
        net_all_loop_result = 0;
    } else {
        printf(
            "  数据不匹配: 期望=\"%s\", 实际=\"%s\"\n",
            NET_ALL_LOOPBACK_MESSAGE,
            buffer
        );
        net_all_loop_result = 5;
    }

done:
    if (socket_fd >= 0) {
        close(socket_fd);
    }
    (void) rtems_semaphore_release(net_all_loop_done);
    (void) rtems_task_suspend(RTEMS_SELF);
}

static int net_all_run_loopback_test(void)
{
    rtems_status_code sc;
    int result = -1;

    printf("\n[1/3] 容器内 UDP loopback 测试\n");
    printf("BSD 网络栈已由 net_all 统一初始化\n");
    net_all_loop_container = NULL;
    net_all_loop_task_id = 0;
    net_all_loop_done = 0;
    net_all_loop_result = -1;

    if (net_all_create_binary('L', 'O', 'O', 'P', &net_all_loop_done) != 0) {
        printf("  [FAIL] 创建 loopback 测试信号量失败\n");
        goto cleanup;
    }

    printf("\n创建网络容器...\n");
    net_all_loop_container = rtems_net_container_create();
    if (net_all_loop_container == NULL) {
        printf("  [FAIL] 创建 loopback 测试容器失败\n");
        goto cleanup;
    }
    printf(
        "容器创建成功 (ID=%d, rc=%d)\n",
        net_all_loop_container->containerID,
        net_all_loop_container->rc
    );
    printf("  NetContainer: %p\n", (void *) net_all_loop_container);
    printf("  net_group: %p\n", (void *) net_all_loop_container->group);

    printf("\n创建测试任务...\n");
    sc = rtems_task_create(
        rtems_build_name('L', 'O', 'O', 'P'),
        2,
        NET_ALL_SOCKET_STACK_SIZE,
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        &net_all_loop_task_id
    );
    if (sc != RTEMS_SUCCESSFUL ||
        net_all_move_task(net_all_loop_task_id, net_all_loop_container) != 0) {
        printf("  [FAIL] 创建或迁移 loopback 测试任务失败\n");
        goto cleanup;
    }
    printf("任务创建成功 (ID=0x%08x)\n", (unsigned) net_all_loop_task_id);
    printf("\n关联任务到容器...\n");
    printf("任务已关联到网络容器\n");
    printf(
        "  新 NetContainer: %p (ID=%d, rc=%d)\n",
        (void *) net_all_loop_container,
        net_all_loop_container->containerID,
        net_all_loop_container->rc
    );

    printf("\n启动测试任务...\n");
    sc = rtems_task_start(net_all_loop_task_id, net_all_loop_task, 0);
    if (sc != RTEMS_SUCCESSFUL) {
        printf("  [FAIL] 启动 loopback 测试任务失败\n");
        goto cleanup;
    }

    (void) rtems_semaphore_obtain(net_all_loop_done, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    result = net_all_loop_result;
    printf("\n═══════════════════════════════════════\n");
    printf("           测试结果\n");
    printf("═══════════════════════════════════════\n");
    if (result == 0) {
        printf("测试通过!\n");
        printf("  [PASS] 127.0.0.1:%d UDP 自发自收成功\n", NET_ALL_LOOPBACK_PORT);
    } else {
        printf("测试失败 (错误码: %d)\n", result);
    }
    printf("═══════════════════════════════════════\n");

cleanup:
    net_all_delete_task(&net_all_loop_task_id);
    net_all_delete_semaphore(&net_all_loop_done);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Port isolation test (formerly net_port.c).                                */
/* ------------------------------------------------------------------------- */

static NetContainer *net_all_port_container[2];
static rtems_id net_all_port_task_id[2];
static rtems_id net_all_port_main_ready;
static rtems_id net_all_port_peer_done;
static rtems_id net_all_port_done[2];
static volatile int net_all_port_result[2];

static int net_all_bind_udp_tcp(
    const char *container_name,
    int *udp_socket,
    int *tcp_socket
)
{
    struct sockaddr_in address;
    int bind_result;

    *udp_socket = -1;
    *tcp_socket = -1;
    printf(
        "\n [%s] 尝试绑定 UDP + TCP 到端口 %d\n",
        container_name,
        NET_ALL_SHARED_PORT
    );
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(NET_ALL_SHARED_PORT);

    printf("  [%s] 创建 UDP socket...\n", container_name);
    *udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (*udp_socket < 0) {
        printf("   [%s] UDP socket() 失败: %s\n", container_name, strerror(errno));
        goto fail;
    }
    printf("   [%s] UDP socket 创建成功 (fd=%d)\n", container_name, *udp_socket);
    bind_result = bind(*udp_socket, (struct sockaddr *) &address, sizeof(address));
    if (bind_result != 0) {
        printf("   [%s] UDP bind() 失败: %s\n", container_name, strerror(errno));
        if (errno == EADDRINUSE) {
            printf("       端口被占用！容器隔离可能失败\n");
        }
        goto fail;
    }
    printf(
        "   [%s] UDP 绑定成功: 127.0.0.1:%d\n",
        container_name,
        NET_ALL_SHARED_PORT
    );

    printf("   [%s] 创建 TCP socket...\n", container_name);
    *tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*tcp_socket < 0) {
        printf("   [%s] TCP socket() 失败: %s\n", container_name, strerror(errno));
        goto fail;
    }
    printf("   [%s] TCP socket 创建成功 (fd=%d)\n", container_name, *tcp_socket);
    bind_result = bind(*tcp_socket, (struct sockaddr *) &address, sizeof(address));
    if (bind_result != 0) {
        printf("   [%s] TCP bind() 失败: %s\n", container_name, strerror(errno));
        if (errno == EADDRINUSE) {
            printf("       端口被占用！容器隔离可能失败\n");
        }
        goto fail;
    }
    printf(
        "   [%s] TCP 绑定成功: 127.0.0.1:%d\n",
        container_name,
        NET_ALL_SHARED_PORT
    );
    printf(
        "   [%s] UDP + TCP 都成功绑定到端口 %d\n",
        container_name,
        NET_ALL_SHARED_PORT
    );
    return 0;

fail:
    if (*udp_socket >= 0) {
        close(*udp_socket);
        *udp_socket = -1;
    }
    if (*tcp_socket >= 0) {
        close(*tcp_socket);
        *tcp_socket = -1;
    }
    return -1;
}

static rtems_task net_all_port_task(rtems_task_argument argument)
{
    int index = (int) argument;
    int udp_socket;
    int tcp_socket;
    Thread_Control *executing = _Thread_Get_executing();
    NetContainer *container = executing->container->netContainer;
    const char *name = index == 0 ? "根测试容器" : "子容器";

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf(" %s任务开始\n", name);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf(" %s任务运行在网络容器 (ID=%d)\n", name, container->containerID);

    if (index == 1) {
        printf(" 等待根测试容器完成绑定...\n");
        (void) rtems_semaphore_obtain(
            net_all_port_main_ready,
            RTEMS_WAIT,
            RTEMS_NO_TIMEOUT
        );
    }

    net_all_port_result[index] =
        net_all_bind_udp_tcp(name, &udp_socket, &tcp_socket);

    if (index == 1 && net_all_port_result[index] == 0) {
        printf("\n 子容器端口绑定成功！\n");
        printf(
            "   根测试容器和子容器同时使用端口 %d，无冲突\n",
            NET_ALL_SHARED_PORT
        );
    }

    if (index == 0) {
        (void) rtems_semaphore_release(net_all_port_main_ready);
        (void) rtems_semaphore_obtain(
            net_all_port_peer_done,
            RTEMS_WAIT,
            RTEMS_NO_TIMEOUT
        );
    } else {
        (void) rtems_semaphore_release(net_all_port_peer_done);
    }

    if (udp_socket >= 0) {
        close(udp_socket);
    }
    if (tcp_socket >= 0) {
        close(tcp_socket);
    }

    printf("\n %s任务完成\n", name);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    (void) rtems_semaphore_release(net_all_port_done[index]);
    (void) rtems_task_suspend(RTEMS_SELF);
}

static int net_all_run_port_test(void)
{
    rtems_status_code sc;
    int result = -1;
    int i;

    printf("\n[2/3] UDP/TCP 端口隔离测试\n");
    printf("BSD 网络栈已由 net_all 统一初始化\n\n");
    memset(net_all_port_container, 0, sizeof(net_all_port_container));
    memset(net_all_port_task_id, 0, sizeof(net_all_port_task_id));
    memset(net_all_port_done, 0, sizeof(net_all_port_done));
    net_all_port_result[0] = -1;
    net_all_port_result[1] = -1;
    net_all_port_main_ready = 0;
    net_all_port_peer_done = 0;

    if (net_all_create_binary('P', 'R', 'D', 'Y', &net_all_port_main_ready) != 0 ||
        net_all_create_binary('P', 'P', 'E', 'R', &net_all_port_peer_done) != 0 ||
        net_all_create_binary('P', 'D', 'N', '1', &net_all_port_done[0]) != 0 ||
        net_all_create_binary('P', 'D', 'N', '2', &net_all_port_done[1]) != 0) {
        printf("  [FAIL] 创建端口测试信号量失败\n");
        goto cleanup;
    }

    printf(" 创建网络容器...\n");
    net_all_port_container[0] = rtems_net_container_create();
    net_all_port_container[1] = rtems_net_container_create();
    if (net_all_port_container[0] == NULL || net_all_port_container[1] == NULL) {
        printf("  [FAIL] 创建端口测试容器失败\n");
        goto cleanup;
    }
    printf(" 网络容器创建成功:\n");
    printf("   - 根测试容器 (ID=%d)\n", net_all_port_container[0]->containerID);
    printf("   - 子容器 (ID=%d)\n", net_all_port_container[1]->containerID);

    for (i = 0; i < 2; ++i) {
        sc = rtems_task_create(
            rtems_build_name('P', 'O', 'R', (char) ('1' + i)),
            2,
            NET_ALL_SOCKET_STACK_SIZE,
            RTEMS_DEFAULT_MODES,
            RTEMS_DEFAULT_ATTRIBUTES,
            &net_all_port_task_id[i]
        );
        if (sc != RTEMS_SUCCESSFUL ||
            net_all_move_task(net_all_port_task_id[i], net_all_port_container[i]) != 0) {
            printf("  [FAIL] 创建或迁移端口测试任务%d失败\n", i + 1);
            goto cleanup;
        }
        printf(
            " 任务%d已关联到网络容器 (ID=%d, rc=%d)\n",
            i + 1,
            net_all_port_container[i]->containerID,
            net_all_port_container[i]->rc
        );
    }

    printf("\n 启动根测试容器和子容器任务...\n");
    for (i = 0; i < 2; ++i) {
        sc = rtems_task_start(net_all_port_task_id[i], net_all_port_task, i);
        if (sc != RTEMS_SUCCESSFUL) {
            printf("  [FAIL] 启动端口测试任务%d失败\n", i + 1);
            goto cleanup;
        }
    }

    (void) rtems_semaphore_obtain(net_all_port_done[0], RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    (void) rtems_semaphore_obtain(net_all_port_done[1], RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    result = net_all_port_result[0] == 0 && net_all_port_result[1] == 0 ? 0 : -1;
    printf("\n                        测试结果\n");
    printf("测试项目:\n");
    printf("  1. 根测试容器 UDP 绑定: %s\n", net_all_port_result[0] == 0 ? "成功" : "失败");
    printf("  2. 根测试容器 TCP 绑定: %s\n", net_all_port_result[0] == 0 ? "成功" : "失败");
    printf("  3. 子容器 UDP 绑定: %s\n", net_all_port_result[1] == 0 ? "成功" : "失败");
    printf("  4. 子容器 TCP 绑定: %s\n", net_all_port_result[1] == 0 ? "成功" : "失败");
    if (result == 0) {
        printf("\n 测试通过!\n\n");
        printf("验证项目:\n");
        printf("   根测试容器成功绑定 UDP + TCP 到端口 %d\n", NET_ALL_SHARED_PORT);
        printf("   子容器成功绑定 UDP + TCP 到相同端口 %d\n", NET_ALL_SHARED_PORT);
        printf("   无 EADDRINUSE (端口已被使用) 错误\n");
        printf("   容器间端口表完全隔离\n");
        printf("   网络资源隔离正常工作\n");
    } else {
        printf("\n 测试失败!\n");
        printf("   可能原因: 容器端口表未正确隔离\n");
    }

cleanup:
    printf("\n 清理测试资源...\n");
    for (i = 0; i < 2; ++i) {
        net_all_delete_task(&net_all_port_task_id[i]);
    }
    net_all_delete_semaphore(&net_all_port_main_ready);
    net_all_delete_semaphore(&net_all_port_peer_done);
    net_all_delete_semaphore(&net_all_port_done[0]);
    net_all_delete_semaphore(&net_all_port_done[1]);
    printf(" socket、任务和信号量清理完成\n");
    printf("                        测试结束\n");
    return result;
}

static rtems_task Init(rtems_task_argument argument)
{
    int results[3];
    int passed = 0;
    int i;

    (void) argument;
    TEST_BEGIN();

    printf("初始化 BSD 网络栈...\n");
    if (rtems_bsdnet_initialize_network() < 0) {
        printf("[FAIL] BSD 网络栈初始化失败: %s\n", strerror(errno));
        TEST_END();
        rtems_test_exit(1);
    }
    printf("[PASS] BSD 网络栈初始化成功\n");

    results[1] = net_all_run_loopback_test();
    results[2] = net_all_run_port_test();
    results[0] = net_all_run_interface_test();

    for (i = 0; i < 3; ++i) {
        if (results[i] == 0) {
            ++passed;
        }
    }

    printf("\n网络容器综合测试结果: %d/3 通过\n", passed);
    printf("  接口隔离: %s\n", results[0] == 0 ? "PASS" : "FAIL");
    printf("  UDP loopback: %s\n", results[1] == 0 ? "PASS" : "FAIL");
    printf("  端口隔离: %s\n", results[2] == 0 ? "PASS" : "FAIL");

    TEST_END();
    rtems_test_exit(passed == 3 ? 0 : 1);
}

struct rtems_bsdnet_config rtems_bsdnet_config = {
    NULL, NULL, 0, 0, 0, 0, 0, 0, 0,
    { "0.0.0.0" }, { "0.0.0.0" }, 0, 0, 0, 0, 0
};

struct rtems_bsdnet_config *rtems_bsdnet_config_ptr = &rtems_bsdnet_config;

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_LIBBLOCK
#define CONFIGURE_APPLICATION_NEEDS_LIBNETWORKING

#define CONFIGURE_MAXIMUM_DRIVERS 10
#define CONFIGURE_MAXIMUM_TASKS 10
#define CONFIGURE_MAXIMUM_SEMAPHORES 10
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 40
#define CONFIGURE_MAXIMUM_SOCKETS 20
#define CONFIGURE_MAXIMUM_REGIONS 10
#define CONFIGURE_EXECUTIVE_RAM_SIZE (2 * 1024 * 1024)

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT

#include <rtems/confdefs.h>
