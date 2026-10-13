# 统一容器 OCI 创建参数说明

本文整理 RTContainer 统一容器创建接口与 OCI Runtime Specification 之间的参数关系，供实现 OCI 适配层时使用。代码基准为提交 `a5d824541f`，核对日期为 2026-10-13。

对外创建操作接收容器 ID 和 bundle 路径；适配层读取 `config.json`，将支持的隔离及资源配置转换为 `RtemsContainerConfig`，并保存应用启动信息。现有统一容器接口只创建隔离环境和资源控制对象，不创建或启动应用任务。

本文中的字段对应关系是适配设计依据，不表示当前 RTEMS 后端已经实现完整 OCI 语义。尤其不能仅凭字段名称相同，就直接透传 CPU、内存或 I/O 配置。

## 1. 对外创建接口

OCI 定义的创建操作输入为：

```text
create <container-id> <path-to-bundle>
```

| 输入 | 含义 | 适配层职责 |
| --- | --- | --- |
| `container-id` | 运行时范围内唯一的容器标识 | 校验唯一性，建立 ID、统一容器句柄和任务之间的映射 |
| `path-to-bundle` | bundle 所在目录 | 读取其中的 `config.json`，解析相对路径，保存配置快照 |

创建阶段准备运行环境，用户程序在 `start <container-id>` 时执行。创建完成后修改磁盘上的 `config.json` 不应改变已创建容器的配置。参见 [OCI 创建操作](https://github.com/opencontainers/runtime-spec/blob/main/runtime.md#create)和[生命周期](https://github.com/opencontainers/runtime-spec/blob/main/runtime.md#lifecycle)。

bundle 中的主要配置如下。应用启动字段通常随创建配置提交，但执行应用必须留到 `start` 阶段。

| 配置字段 | 用途 | 当前统一容器的承接情况 |
| --- | --- | --- |
| `ociVersion` | 声明采用的 OCI Runtime Spec 版本 | 由适配层校验 |
| `root.path`、`root.readonly` | 根文件系统路径及只读属性 | 没有对应创建字段，需要文件系统适配 |
| `mounts[]` | 每项包含 `destination`、`source`、`type`、`options` 等 | MNT 开关只创建挂载命名空间，具体挂载需要另行配置 |
| `hostname` | 容器主机名 | 对应 `uts_name`，需要启用 UTS |
| `process.args`、`process.env`、`process.cwd` | 应用及参数、环境变量、工作目录 | 没有对应创建字段，需要任务启动适配 |
| `process.user` | UID、GID 等身份配置 | 没有对应统一创建字段，需要单独定义后端支持范围 |
| `process.terminal` | 是否为应用分配终端 | 没有对应统一创建字段，需要终端和标准流适配 |
| `linux.namespaces[]` | 命名空间类型及可选的现有 namespace 路径 | 部分类型可映射为 `flags`；加入已有 namespace 尚无统一创建参数 |
| `linux.resources.cpu` | CPU 权重、配额和周期等 | 部分字段可转换为 core cgroup 配置，见第 4 节 |
| `linux.resources.memory` | 内存资源限制 | `limit` 可作为堆分配配额输入，但控制范围不同 |
| `linux.resources.blockIO` | I/O 权重、每设备带宽或 IOPS 限制 | 当前仅有组级 I/O 配置，不能直接实现全部语义 |
| `annotations` | 字符串键值形式的扩展元数据 | 可约定 RTEMS 应用标识、栈大小等扩展，见第 5 节 |

字段定义见 [OCI 通用配置](https://github.com/opencontainers/runtime-spec/blob/main/config.md)及 [Linux 平台配置](https://github.com/opencontainers/runtime-spec/blob/main/config-linux.md)。`linux.*` 是 Linux 平台字段；在 RTEMS 上使用这些输入时，应声明实际支持的字段和行为。表中未展开的 hooks、安全配置和其他资源字段，不能因未列出而视为已支持。

## 2. 当前底层接口与全部参数

当前创建入口为：

```c
rtems_status_code rtems_unified_container_create(
    const RtemsContainerConfig *config,
    RtemsContainer **container
);
```

`config` 是输入配置，`container` 是输出句柄。字段定义见 [container.h](../cpukit/include/rtems/score/container.h#L128)，其中 core cgroup 子结构定义见 [corecgroup.h](../cpukit/include/rtems/score/corecgroup.h#L50)。

下表完整列出创建配置中的字段。默认值来自 [rtems_unified_container_config_initialize](../cpukit/score/src/container.c#L178)，表示当前代码默认值，并非 OCI 规定的默认值。`1 MiB = 1048576` 字节。

| 字段 | C 类型 | 当前默认值 | 实际用途与单位 |
| --- | --- | --- | --- |
| `flags` | `uint32_t` | `RTEMS_UNIFIED_CONTAINER_ALL` | 选择命名空间和资源组件 |
| `uts_name` | `const char *` | `"container"` | UTS 主机名 |
| `cgroup_config.cpu_shares` | `uint64_t` | `1` | CPU 相对权重字段；当前未见调度执行路径使用 |
| `cgroup_config.cpu_quota` | `uint64_t` | `100` | 每周期 CPU 时间额度，实际单位为 RTEMS tick |
| `cgroup_config.cpu_period` | `uint64_t` | `1000` | CPU 额度补充周期，实际单位为 RTEMS tick |
| `cgroup_config.memory_limit` | `uint64_t` | `1048576` | 关联线程的堆分配配额，字节 |
| `cgroup_config.blkio_limit` | `uint64_t` | `1048576` | 原有 I/O 限制字段；当前未见执行路径使用 |
| `io_system_read_bps` | `uint64_t` | `1048576` | 全局 I/O 管理器总读带宽，字节/秒 |
| `io_system_write_bps` | `uint64_t` | `1048576` | 全局 I/O 管理器总写带宽，字节/秒 |
| `io_read_bps_limit` | `uint64_t` | `1048576` | 组级读带宽输入，字节/秒；创建过程中会被重算 |
| `io_write_bps_limit` | `uint64_t` | `1048576` | 组级写带宽输入，字节/秒；创建过程中会被重算 |
| `io_window_size_ms` | `uint32_t` | `1000` | I/O 统计窗口，毫秒；创建时被强制设为 1000 |
| `io_weight` | `uint16_t` | `100` | 容器 I/O 权重，必须非零 |
| `io_thread_weight` | `uint32_t` | `100` | 线程加入 I/O 组时分配的权重，必须非零 |

`cgroup_config` 仅在启用 `RTEMS_CGROUP` 时存在，七个 `io_*` 字段仅在启用 `RTEMSCFG_IO_CGROUP` 时存在。统一接口使用的是 `CORE_cgroup_config`，不是另一个公共类型 `rtems_cgroup_config`；后者的 `max_tasks` 不属于这里的创建参数。

传入 `config == NULL` 时，创建函数使用整套默认配置。传入非空结构体时，函数不会逐字段补默认值；适配层可以先调用初始化函数，再按请求覆盖字段，但必须重新选择 `flags` 并处理 OCI 缺省资源的语义。默认的 CPU 和内存数值是实际限制，不能自动等同于“不限额”。

## 3. 命名空间和资源开关

| `flags` 位 | 数值 | 配置来源或作用 |
| --- | --- | --- |
| `RTEMS_UNIFIED_CONTAINER_PID` | `1u << 0` | `namespaces` 中的 `pid` |
| `RTEMS_UNIFIED_CONTAINER_IPC` | `1u << 1` | `namespaces` 中的 `ipc` |
| `RTEMS_UNIFIED_CONTAINER_MNT` | `1u << 2` | `namespaces` 中的 `mount` |
| `RTEMS_UNIFIED_CONTAINER_NET` | `1u << 3` | `namespaces` 中的 `network` |
| `RTEMS_UNIFIED_CONTAINER_UTS` | `1u << 4` | `namespaces` 中的 `uts` |
| `RTEMS_UNIFIED_CONTAINER_CPU` | `1u << 5` | CPU 资源配置 |
| `RTEMS_UNIFIED_CONTAINER_MEM` | `1u << 6` | 内存资源配置 |
| `RTEMS_UNIFIED_CONTAINER_IO` | `1u << 7` | I/O 资源配置 |

这些位定义在 [container.h](../cpukit/include/rtems/score/container.h#L108)。默认 `ALL` 不会自动过滤未编译的组件；请求未编译功能会返回 `RTEMS_NOT_CONFIGURED`，见 [flags 检查](../cpukit/score/src/container.c#L75)。

当前统一接口对选中的 namespace 新建实例，在 `enter` 时切换对应组件；未选中的 namespace 不改变线程原有环境。因此，适配器需要保证承载任务的初始环境符合预期。没有 `user`、`time` 或 `cgroup` namespace 对应位，也没有用于加入已有 namespace 的路径参数。

MNT 创建固定继承全局根容器的挂载表，见 [MNT 创建分支](../cpukit/score/src/container.c#L278)。启用 MNT 不代表已经将 `root.path` 设置为容器根目录，也不代表已经执行 `mounts[]` 或只读策略。

NET 创建包含独立 loopback，veth 由应用另行创建，见 [netContainer.c](../cpukit/score/src/netContainer.c#L313)。IP、网关、veth 和 DNS 不属于当前统一创建参数，也不是设置 OCI `network` namespace 后就会自动完成的配置；如需这些能力，应由单独的网络配置流程负责。

## 4. 资源参数的转换与限制

### CPU 和内存

OCI `cpu.quota`、`cpu.period` 的时间单位为微秒。虽然公共 cgroup 头文件中有微秒注释，统一接口实际使用的后端将数值直接交给 tick watchdog，并按 tick 差值扣除额度，见 [corecgroupimpl.h](../cpukit/include/rtems/score/corecgroupimpl.h#L122) 和 [threaddispatch.c](../cpukit/score/src/threaddispatch.c#L299)。

对于正的有限额度，转换关系为：

```text
RTEMS ticks = OCI microseconds × ticks_per_second / 1000000
```

例如系统 tick 为 1 ms 时，OCI 的 `quota = 20000`、`period = 100000` 对应后端的 `20`、`100` tick。适配层还需处理整数溢出、非整 tick 值、低于一个 tick 的请求，以及缺省或不限额输入；应明确拒绝条件或精度策略，不能直接强转成无符号数。

CPU/MEM 当前共用完整 core cgroup。任一位开启，都会创建并挂入该对象；初始化同时设置 CPU watchdog 和内存额度，见 [统一创建分支](../cpukit/score/src/container.c#L310) 和 [core cgroup 初始化](../cpukit/score/src/corecgroup.c#L61)。所以只选择 MEM 仍可能受到 CPU 默认配额限制，只选择 CPU 也仍有内存配额，尚不能把这两个位解释为独立的控制器开关。

`memory_limit` 的当前执行范围是关联线程的堆分配记账，见 [heapallocate.c](../cpukit/score/src/heapallocate.c)。它不能直接等同于 Linux 内存 cgroup 对完整进程内存的限制。`cpu_shares` 和 `blkio_limit` 虽在配置结构中存在，但在当前代码基准中未发现实际控制路径，不应宣称已经实现相应资源控制。

### I/O

`io_system_read_bps`、`io_system_write_bps` 描述全局管理器的带宽，建议作为运行时启动配置，而不是每个容器独立声明的额度。管理器已存在时，再次调用创建函数不会更新这两个值；首次传入 0 则替换为 1 MiB/s，见 [I/O 管理器创建](../cpukit/score/src/io_cgroup.c#L12)。

当前 I/O 创建过程有两项覆盖行为：

1. 复制传入限制后，将 `window_size_ms` 强制设置为 `1000`，见 [I/O 组初始化](../cpukit/score/src/io_cgroup.c#L119)。
2. 分配权重时，重算所有组的读写限制，覆盖传入的 `io_read_bps_limit` 和 `io_write_bps_limit`，见 [权重分配](../cpukit/score/src/io_cgroup.c#L361)。

当前重算公式为：

```text
组读带宽 = 组权重 × 系统总读带宽 / 全部组权重之和
组写带宽 = 组权重 × 系统总写带宽 / 全部组权重之和
```

因此，传入的组级带宽还不是独立于权重的硬上限。创建后调用设置限制接口，也可能在后续权重或全局带宽变化时再次被覆盖。

`io_weight` 和 `io_thread_weight` 均需大于 0，组内已分配线程权重之和不能超过组权重，见 [线程权重检查](../cpukit/score/src/io_cgroup.c#L218)。当前统一对象直接附着一个线程时，应满足 `io_thread_weight <= io_weight`。

OCI `blockIO.throttleReadBpsDevice`、`throttleWriteBpsDevice` 按设备的 `major/minor` 指定速率。当前统一配置没有设备维度和 IOPS 字段，不能通过丢弃设备号、合并数值的方式声称实现相同语义。参见 [OCI Block IO 定义](https://github.com/opencontainers/runtime-spec/blob/main/config-linux.md#block-io)。

## 5. RTEMS 应用启动扩展

当前示例先创建统一容器，再单独调用 `rtems_task_create()` 和 `rtems_task_start()`，任务执行后调用 `rtems_unified_container_enter()`，见 [container06 示例](../testsuites/container/container06/init.c#L234)。完整的 OCI 启动适配还需定义以下信息的来源：

| 信息 | 建议来源 | 说明 |
| --- | --- | --- |
| 应用入口 | `process.args[0]` 对应的注册表或模块加载器；必要时增加扩展应用标识 | 由运行时解析入口，不在 JSON 中传函数指针 |
| 参数和环境 | `process.args`、`process.env` | 由启动适配器传递并落实所声明的隔离范围 |
| 工作目录、身份、标准流 | `process.cwd`、`process.user`、`process.terminal` 及运行时 I/O 选项 | 当前统一创建接口不处理 |
| RTEMS 任务优先级 | 运行时默认值或 RTEMS 扩展 | 不能直接假定与 Linux 调度优先级数值同义 |
| RTEMS 任务栈大小 | 运行时默认值或 RTEMS 扩展，单位约定为字节 | 不属于当前 `RtemsContainerConfig` |
| 任务 modes、attributes | 运行时默认策略；有需求时增加扩展 | 来自 `rtems_task_create()` 的启动参数 |

如通过 `annotations` 承载扩展，应使用项目约定的键名及字符串值，例如 `com.example.rtcontainer.stack-size`。这只是自定义协议示例，不是 OCI 标准字段，也不是当前已经实现的配置解析器。应用入口注册或动态模块加载还需验证目标程序与 RTEMS 的兼容性。

## 6. 适配层的创建和启动流程

1. 接收容器 ID 和 bundle 路径，读取并保存配置快照，校验 ID、字段、编译能力及参数可表达范围。
2. 生成 `RtemsContainerConfig`，转换时间单位，处理缺省资源策略，拒绝后端无法落实的配置。
3. 调用 `rtems_unified_container_create()`，获得隔离环境和资源对象。
4. 完成根文件系统、挂载、网络等已声明支持的环境配置；准备受启动门闩控制的承载任务，使其进入容器并完成创建阶段所需配置，保证用户应用入口尚未执行。
5. 保存容器运行时对象并进入 `created` 状态。收到 `start` 后，完成允许延后设置的 `process` 配置，并放行用户应用入口。
6. 跟踪应用退出，落实终止、清理和状态查询。创建失败时回收本次已分配资源。

适配层至少需要保存：容器 ID、bundle 路径、配置快照、`RtemsContainer *`、任务 ID、应用启动描述和生命周期状态。这些是运行时管理数据，不应全部塞进底层资源配置结构。

现有生命周期接口也需要区分：

- `enter()` 附着一个已有线程，不等于创建应用；统一对象的 `attached_thread` 限制其直接附着第二个不同线程，见 [enter 检查](../cpukit/score/src/container.c#L398)。
- `leave()` 将线程移出容器，不会终止任务。
- `delete()` 对附着线程调用 `leave()` 后释放容器资源，不会自动终止应用任务，见 [delete 实现](../cpukit/score/src/container.c#L668)。
- `pause()`、`resume()` 依赖 core cgroup，不能作为所有配置都支持的通用操作，见 [pause 实现](../cpukit/score/src/container.c#L620)。

统一层没有整体 `update_config` 接口，修改创建时的原始结构体不会更新已创建的容器。OCI 层的配置更新、多个任务的成员管理和完整生命周期，需要另行实现并验证。
