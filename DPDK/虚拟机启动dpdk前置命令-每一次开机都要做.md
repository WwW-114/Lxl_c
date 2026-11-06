配置大页内存（Hugepages）

DPDK 需要大页内存来减少地址转换开销。你可以在终端里动态分配一些 2MB 的大页（例如分配 1024 个，即 2GB 内存）：



\# 分配 1024 个 2MB 的大页

**echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr\_hugepages**



\# 挂载 hugetlbfs 文件系统（如果还没挂载的话）

**sudo mkdir -p /mnt/huge （没有目录才创造，有则跳过）**

**sudo mount -t hugetlbfs nodev /mnt/huge**



你可以用 **grep Huge /proc/meminfo** 来确认大页是否分配成功。



加载 VFIO 驱动并绑定网卡



\# 加载 vfio-pci 驱动，并开启无 IOMMU 模式（虚拟机环境必需）

**sudo modprobe vfio-pci**

**e**cho 1 | sudo tee /sys/module/vfio/parameters/enable\_unsafe\_noiommu\_mode



\# 用新版 DPDK 自带的工具绑定网卡（进入你的 DPDK 源码目录下的 usertools）

**cd ~/dpdk-stable-24.11.7/usertools （这里的路径因环境不同也需要改变）**

**sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0**  # 替换成你实际的 PCI 地址



绑定后，用 **./dpdk-devbind.py --status**（同时也能查看对应的网卡的PCI地址） 看到网卡在 using DPDK-compatible driver 一栏下就对了。

