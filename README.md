Real-Time Container based on RTEMS6 for Phytium-d2000
===============================================

## 1. Real-Time Executive for Multiprocessing Systems
Project git repositories are located at:

  * https://gitlab.rtems.org/rtems/


Online documentation is available at:

  * https://docs.rtems.org/


RTEMS Doxygen for CPUKit:

  * https://docs.rtems.org/doxygen/branches/master/


RTEMS POSIX 1003.1 Compliance Guide:

  * https://docs.rtems.org/branches/master/posix-compliance/


RTEMS Mailing Lists for general purpose use the users list and for developers 
use the devel list.

  * https://lists.rtems.org/mailman/listinfo


The version number for this software is indicated in the VERSION file.

## 2. Startup
step.1 pull RTEMS6 development environment
```bash
docker pull roker405/rtems6-env:v1.0
docker run -it --name rtems6-dev \
  --net=host \
  roker405/rtems6-env:v1.0 \
  bash
docker exec -it -e "TERM=xterm-256color" rtems6-dev bash
```

step.2 pull phytium version source code
```bash
cd /usr/src
git clone https://github.com/The-Lyc/RTContainer.git rtems6
```

step.3 set config.ini
```bash
cd rtems6
cat << EOF > config.ini
[DEFAULT]
BUILD_TESTS = True
RTEMS_POSIX_API = True

[aarch64/a53_lp64_qemu]
EOF
```

step.4 build bsp
```bash
./waf configure --prefix=/opt/rtems6 
# append -j to use more cpu cores
./waf build 
./waf install
```

step.5 use qemu to virtually run target elf
```bash
apt install qemu-system-aarch64
qemu-system-aarch64 -M virt,gic-version=3 \
  -cpu cortex-a53  -smp 1  -m 512M        \
  -nographic  -no-reboot   -kernel        \
  build/aarch64/a53_lp64_qemu/testsuites/samples/hello.exe
```

## 3. Container documentation

- [统一容器 OCI 创建参数说明](docs/oci-unified-container-parameters.md)：创建输入、字段默认值、OCI 映射与当前实现限制。
