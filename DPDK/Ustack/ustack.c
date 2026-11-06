/**
 * @file  ustack.c
 * @brief 基于 DPDK 的最小用户态协议栈实验：
 *        轮询 VMware vmxnet3 网卡，解析 Ethernet + IPv4 + UDP/TCP，
 *        打印 payload，并把 payload 原样回显给发送端。
 *        - UDP：无状态，收到一个包就交换地址并回显；
 *        - TCP：单连接、最小状态机（SYN/SYN-ACK/ACK、数据 ACK、FIN|ACK），
 *          不实现重传、拥塞控制、窗口缩放和多连接；
 *        - ICMP：对 Echo Request 回 Echo Reply，支持发送端 ping。
 *
 * ============================ 运行环境 ============================
 *   - 虚拟机：VMware Workstation 中的 Ubuntu
 *   - 网卡  ：VMware vmxnet3（PCI 0000:0b:00.0，具体以本机为准）
 *   - DPDK  ：24.11.7，源码路径 ~/dpdk-stable-24.11.7
 *   - 源码  ：/mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
 *             （Windows 侧 D:\daimacheshi\cpp_internet_study\DPDK\Ustack）
 *
 * ============================ 编译与运行 ============================
 *   编译：
 *       make
 *   运行（需要 root，因为要访问 vfio 设备和大页内存）：
 *       sudo ./build/ustack --iova-mode=pa
 *
 *   为什么必须带 --iova-mode=pa：
 *       虚拟机里 vfio 以 no-IOMMU（unsafe）模式工作，没有 IOMMU 做虚拟地址到
 *       物理地址的转换，网卡 DMA 只能使用物理地址（PA）。因此 EAL 必须选择
 *       PA 模式；否则 vmxnet3 可能初始化成功但收发异常。
 *
 * ============================ 构建问题总结 ============================
 *   Makefile 复制自 DPDK 源码中的示例：
 *       <DPDK 源码根目录>/examples/helloworld/Makefile
 *   本环境 DPDK 源码根目录是 ~/dpdk-stable-24.11.7。
 *   复制后做了三处修改：
 *
 *   【修改 1】修改生成的可执行程序名
 *       原内容：APP = helloworld
 *       修改为：APP = ustack
 *
 *   【修改 2】修改参与编译的源文件名
 *       原内容：SRCS-y := main.c
 *       修改为：SRCS-y := ustack.c
 *       说明：源文件名必须与实际文件 ustack.c 完全一致。
 *             此前曾误写为 usatck.c，导致 make 报
 *             “没有规则可制作目标 usatck.c”。
 *
 *   【修改 3】将软链接改为文件复制，以适配共享文件夹
 *       原内容：
 *           shared: build/$(APP)-shared
 *               ln -sf $(APP)-shared build/$(APP)
 *           static: build/$(APP)-static
 *               ln -sf $(APP)-static build/$(APP)
 *       修改为：
 *           shared: build/$(APP)-shared
 *               cp build/$(APP)-shared build/$(APP)
 *           static: build/$(APP)-static
 *               cp build/$(APP)-static build/$(APP)
 *       说明：原 Makefile 用 ln -sf 创建软链接
 *             build/ustack -> ustack-shared。但 VMware 共享文件夹 hgfs
 *             默认不支持创建符号链接，执行时报：
 *                 ln: Operation not supported
 *                 make: *** [Makefile:20：shared] 错误 1
 *             改为 cp 复制可执行文件，不依赖符号链接，即可正常构建。
 *             clean 目标中原有的 rm -f build/$(APP) 已能清理该文件。
 *
 *   - 通过 pkg-config libdpdk 获取编译/链接参数；若报
 *     "no installation of DPDK found"，检查 DPDK 是否安装及 PKG_CONFIG_PATH；
 *   - hgfs 的“修改时间在未来”警告不影响编译，必要时 touch 源文件/Makefile；
 *   - 本项目只使用 shared 链接（make 默认目标），未使用 static。
 *
 * ============================ 两端网络前提 ============================
 *   1. 发送端网卡与这里绑定的 DPDK 网卡必须处在同一个 VMware VMnet
 *      （同一个虚拟二层交换机）。仅仅 IP 同网段是不够的：
 *      - NAT（VMnet8）与仅主机（VMnet1）是两个不同的虚拟交换机；
 *      - 不同物理主机上的 NAT 虚拟机之间也不通。
 *   2. ARP 已经在 ustack 中实现：发送端不再需要静态邻居。发送端内核会先发
 *      ARP Request，ustack 收到后回 ARP Reply，内核自动缓存 IP-MAC。
 *      如果只是想手工固定表项（调试/对比），仍然可以：
 *          sudo ip neigh replace 10.10.10.100 \
 *               lladdr 00:0C:29:F4:81:B8 nud permanent dev ens37
 *   3. 发送端 IP 直接用 ip addr add 配置即可，不需要 nmcli，也不需要修改
 *      NetworkManager：
 *          sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
 *      然后用 ip -br addr show ens37 确认地址（注意 ip -br link 不显示 IP）。
 *   4. 确认路由确实走测试网卡：
 *          ip route get 10.10.10.100
 *          # 期望：10.10.10.100 dev ens37 src 10.10.10.10
 *   5. ARP 测试（可选；先清掉可能存在的静态/旧表项）：
 *          sudo ip neigh flush dev ens37
 *          sudo arping -I ens37 -c 3 10.10.10.100
 *      期望 arping 收到 3 个 Reply。注意 arping 不写内核邻居表；
 *      要让内核自动学习，用 ping / UDP / TCP（见下一步）。
 *   6. 发送 UDP：
 *          printf 'hello-dpdk' | nc -u -w 1 10.10.10.100 9999
 *      目的 IP 和端口都是任意的：本程序不校验 IP，也不校验端口，
 *      只要二层目的 MAC 是本网卡即可。
 *   7. 回显测试：
 *      ustack 会把 payload 发回“发送端的源端口”。nc -u -w 1 发完就退出，
 *      源端口已经关闭，收不到回显。要用绑定固定源端口并等待的程序，例如：
 *          python3 - <<'PY'
 *          import socket
 *          s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
 *          s.bind(("10.10.10.10", 50000))
 *          s.settimeout(3)
 *          s.sendto(b"hello-dpdk", ("10.10.10.100", 9999))
 *          print(s.recvfrom(2048))
 *          PY
 *   8. TCP 回显测试（可选）：
 *          python3 - <<'PY'
 *          import socket
 *          s = socket.create_connection(("10.10.10.100", 9999), timeout=3)
 *          s.sendall(b"hello-tcp")
 *          print(s.recv(2048))
 *          s.close()
 *          PY
 *      TCP 客户端会先完成三次握手；ustack 作为服务器接收并回显。
 *   9. ICMP 测试（可选）：
 *          ping -c 4 10.10.10.100
 *      第一次 ping 会先触发 ARP；ustack 回 ARP Reply 后即可看到 ICMP 回显。
 *      默认 56 字节 payload 不会分片；ping -s 超过 1472 会分片，
 *      当前不重组分片。
 *
 * ============================ ARP 实现 ============================
 *   1. 本机地址：
 *        USTACK_IPV4_ADDR = 10.10.10.100
 *        gLocalIp   = rte_cpu_to_be_32(USTACK_IPV4_ADDR)  （网络字节序）
 *        gLocalAddr = rte_eth_macaddr_get()              （DPDK 网卡 MAC）
 *
 *   2. ARP 帧格式（Ethernet II + ARP，42 字节，补零到 60）：
 *        Ethernet: dst MAC(6) + src MAC(6) + EtherType(2)=0x0806
 *        ARP     : htype(2)=1 + ptype(2)=0x0800 + hlen(1)=6 + plen(1)=4
 *                  + opcode(2) + sender MAC(6) + sender IP(4)
 *                  + target MAC(6) + target IP(4)
 *
 *   3. 收到 ARP Request（opcode=1）：
 *        - 学习 sender IP/MAC（arp_table_update）；
 *        - target IP == gLocalIp 时回 ARP Reply（opcode=2）：
 *            Ethernet dst = 请求者 MAC，src = 本机 MAC；
 *            ARP sender MAC/IP = 本机，target MAC/IP = 请求者。
 *        - target IP 不是本机时只学习、不应答。
 *
 *   4. 收到 ARP Reply（opcode=2）：
 *        - 学习 sender IP/MAC；当前没有主动 ARP 请求，所以只做缓存。
 *
 *   5. ARP 表：
 *        gArpTable[16]，保存 IP/MAC；当前回显直接使用收到帧里的 MAC，
 *        查表函数是给后续“主动发送/ARP 请求”预留的。
 *
 *   6. 测试：
 *        sudo ip neigh flush dev ens37
 *        sudo arping -I ens37 -c 3 10.10.10.100
 *      注意 arping 不写内核邻居表；要内核自动学习，用 ping / UDP / TCP，
 *      再用 ip neigh show 10.10.10.100 查看缓存。
 *
 * ============================ UDP 回显路径 ============================
 *   1. 交换地址（保留网络字节序，直接按字节拷贝）：
 *        echo.dst_mac  = recv.src_mac   （原发送端 MAC）
 *        echo.src_mac  = recv.dst_mac   （本 DPDK 网卡 MAC）
 *        echo.src_ip   = recv.dst_ip    （10.10.10.100）
 *        echo.dst_ip   = recv.src_ip    （10.10.10.10）
 *        echo.src_port = recv.dst_port  （9999）
 *        echo.dst_port = recv.src_port  （发送端 socket 的源端口）
 *   2. 重新构造 Ethernet(14) + IPv4(20) + UDP(8) 头：
 *        EtherType = 0x0800；
 *        IPv4: version_ihl = 0x45, TTL = 64, protocol = 17,
 *              total_length = 20 + 8 + payload_len；
 *        UDP : dgram_len = 8 + payload_len；
 *        校验和分别由 rte_ipv4_cksum() 和 rte_ipv4_udptcp_cksum() 计算。
 *   3. rte_pktmbuf_alloc() + rte_pktmbuf_append() 申请 TX mbuf，
 *      拷贝 payload，再调用 rte_eth_tx_burst() 发送。
 *   4. TX 成功后 mbuf 归驱动所有，应用不能再 free；只有发送返回 0
 *      （没发出去）时才由应用释放。
 *
 * ============================ TCP 回显路径 ============================
 *   这是一个“单连接、最小可用”的 TCP 回显服务器。
 *
 *     无连接（gTcpConn.vaild == 0）
 *        │ 收到 SYN
 *        ▼
 *     SYN_RCVD
 *        │ 发送 SYN|ACK：seq = iss，ack = client_isn + 1
 *        │ 收到第三次握手的 ACK（纯 ACK，ack = iss + 1）
 *        ▼
 *     ESTABLISHED
 *        │ 收到数据（seq == rcv_nxt）
 *        ├─ 打印 payload
 *        ├─ rcv_nxt += payload_len
 *        ├─ 按 536 字节分片回显：ACK|PSH，
 *        │  seq = snd_nxt，ack = rcv_nxt
 *        └─ snd_nxt += payload_len
 *        │ 收到 FIN（seq + payload_len == rcv_nxt）
 *        ▼
 *     发送 FIN|ACK，rcv_nxt += 1，snd_nxt += 1，连接复位
 *
 *   关键字段：
 *     iss      - 本端初始发送序号（实验固定 0x12345678）
 *     snd_nxt  - 本端下一个发送序号
 *     snd_una  - 对端已确认的本端序号（仅记录，不用于重传）
 *     irs      - 客户端初始发送序号
 *     rcv_nxt  - 本端期望的下一个接收序号
 *
 *   限制：
 *     - 只支持一个连接，新 SYN 覆盖旧连接；
 *     - 只处理按序数据，乱序/重传只回 ACK；
 *     - 不实现重传、拥塞控制、窗口缩放、SACK、时间戳；
 *     - SYN-ACK 固定 20 字节、无选项，对端按默认 MSS 发送；
 *     - 回显按 536 字节分片；
 *     - 不校验接收方向校验和，发送时计算。
 *
 * ============================ TCP 实现中遇到的坑 ============================
 *   1. IP 地址二次 htonl：
 *      conn->local_ip/remote_ip 已经从收到的 IP 头按网络字节序拷贝，
 *      发送时直接赋值，不能再 htonl；否则 SYN-ACK 的源/目的 IP 会字节交换。
 *   2. 端口二次 htons：
 *      连接结构体统一存主机字节序，发送时 htons 一次。
 *      如果在创建连接时存网络字节序、发送时又 htons，SYN-ACK 端口会错位，
 *      客户端无法把 SYN-ACK 匹配到自己的 socket，会一直重传 SYN。
 *   3. 第三次握手是纯 ACK（flags = 0x10），不能要求同时带 SYN。
 *      判断条件应为 (flags & TCP_FLAG_ACK) && ack == snd_nxt。
 *   4. TCP payload 指针必须用 (uint8_t*)tcphdr + tcp_hdr_len；
 *      写成 tcphdr + tcp_hdr_len 会按 struct rte_tcp_hdr 大小放大 20 倍。
 *   5. FIN 必须回 FIN|ACK，只回 FIN 对端可能不认。
 *   6. TCP 校验和必须计算；结果为 0 时写 0xFFFF（TCP 不允许 0）。
 *
 * ============================ ICMP 实现 ============================
 *   1. 只处理 Echo Request（type=8, code=0），回 Echo Reply
 *      （type=0, code=0）。
 *   2. 交换 Ethernet MAC 和 IPv4 地址；ident/seq 原样拷贝，
 *      payload 原样拷贝（ping 用 payload 里的时间戳计算 RTT）。
 *   3. ICMP 消息长度：
 *        ip_len   = IPv4 total_length
 *        icmp_len = ip_len - IHL*4
 *      不能用 pkt_len：短帧会被以太网填充到 60 字节，填充不属于
 *      ICMP payload，也不能算进校验和。
 *   4. ICMP 校验和只覆盖 ICMP 头 + payload，不包含 IP 伪首部；
 *      计算前 icmp_cksum 清零，算完直接写入。
 *   5. 其它 ICMP 类型只打印，不回复；分片 ICMP 直接忽略。
 *
 *   测试：
 *       ping -c 4 10.10.10.100
 *   第一次 ping 会先触发 ARP，ustack 回 ARP Reply 后即可看到回显。
 *
 * ============================ ICMP 实现中遇到的坑 ============================
 *   1. IPv4 头指针加法写错（最隐蔽）：
 *          struct rte_icmp_hdr *icmphdr =
 *              (struct rte_icmp_hdr*)(iphdr + ip_hdr_len);
 *      iphdr 是 struct rte_ipv4_hdr *，+20 实际前进 20*20=400 字节，
 *      读到的是 mbuf 里清零的内存；icmp_type 读到 0，被当成 Echo Reply，
 *      只打印 "ICMP echo reply id=0 seq=0" 而不回包，发送端 ping 不通。
 *      正确写法：
 *          (struct rte_icmp_hdr*)((uint8_t*)iphdr + ip_hdr_len);
 *   2. 回显 payload 来源写错：
 *          rte_memcpy(..., (const uint8_t*)out_iphdr + 8, payload_len);
 *      源指针指向新分配的发送缓冲区，回显 payload 是垃圾数据；
 *      应从请求的 ICMP 头之后拷贝：
 *          (const uint8_t*)icmphdr + sizeof(struct rte_icmp_hdr)
 *   3. 初始长度检查用了 sizeof(struct rte_ipv4_hdr)（20），
 *      应检查 sizeof(struct rte_icmp_hdr)（8）。
 *   4. ICMP 校验和不能用 rte_ipv4_udptcp_cksum()：那个函数会加
 *      IP 伪首部；ICMP 没有伪首部，用 rte_raw_cksum() 自行折叠。
 *
 * ============================ 回显实现中遇到的坑 ============================
 *   1. IPv4 头写成 msg + 1；以太网头 14 字节，应为 msg + 14。
 *      写错会覆盖目的 MAC 和 EtherType，回显帧变成非法帧，
 *      发送端网卡收不到，Wireshark 也解析不出 UDP。
 *   2. version_ihl 写成 0x54（版本 5/IHL 4），应为 0x45（版本 4/IHL 5）。
 *   3. UDP Length 只写了 payload_len，少了 8 字节 UDP 头；
 *      应为 htons(payload_len + sizeof(struct rte_udp_hdr))。
 *   4. 只构造了头部，没有把 data 拷贝到 udphdr + 1，回显 payload 是旧数据。
 *   5. rte_eth_tx_burst() 成功后仍调用 rte_pktmbuf_free(txmbuf)，
 *      造成 use-after-free/double free；只有 nb_tx != 1 时才应释放。
 *   6. TTL 写成 0，部分协议栈会丢包；应设为 64。
 *   7. 测试端用 nc -u 发完就退出，回显目的端口已关闭；
 *      要绑定固定源端口并 recvfrom 等待。
 *
 * ============================ 曾遇到并已修复的问题 ============================
 *   1. dgram_len 是网络字节序，原代码直接当主机字节序用：
 *      UDP 长度 18（0x0012）在小端机器上会被读成 0x1200 = 4608，
 *      随后 %.*s 会按 4600 字节越界读取。必须用
 *      rte_be_to_cpu_16() 或 ntohs() 转换。
 *   2. 原代码用 sizeof(struct rte_udp_hdr *)（指针，8 字节）代替
 *      sizeof(struct rte_udp_hdr)（结构体，8 字节）。x86_64 上碰巧相等，
 *      但语义错误，换平台就会出错。
 *   3. 最隐蔽的一个：收包循环里又调用了一次 rte_eth_rx_burst()，并把它的
 *      返回值（收到的包数量，0/1/2…）当成包长度 pkt_len。结果 pkt_len 恒小于
 *      14+20，所有包都在长度检查处被 free 掉，表现为“完全收不到”。
 *      正确写法是 rte_pktmbuf_data_len(mbufs[i])。
 *   4. iphdr + 1 固定假设 IPv4 头是 20 字节；带 IP 选项时会错位。
 *      正确做法是按 IHL 计算：(version_ihl & 0x0f) * 4。
 *   5. RX 描述符数量原为 128。128 是 vmxnet3 PMD 的最低边界（源码里错误
 *      信息甚至写的是 Min: 256），实际测试中 512 更稳妥，与 testpmd 默认
 *      行为一致；TX 队列配 1 个、512 描述符，用于回显发送。
 *   6. 发送端的静态邻居表项会在接口 down/up 或重新配置后被内核清掉。
 *      典型现象：testpmd 只收到 0x0806 的 ARP 广播，收不到 0x0800 的 UDP；
 *      此时重新执行 ip neigh replace 即可。
 *
 * ============================ 帧格式与偏移 ============================
 *   标准 Ethernet II + IPv4 + UDP/TCP（无 VLAN、无分片、单段 mbuf）：
 *
 *     +------------------+------------------+------------------+--------------+
 *     | Ethernet  14 B   | IPv4  20 B (IHL) | UDP  8 B         | payload      |
 *     +------------------+------------------+------------------+--------------+
 *     | dst MAC    6 B   | version/IHL  1 B | src port    2 B  |              |
 *     | src MAC    6 B   | ...              | dst port    2 B  |              |
 *     | ethertype  2 B   | protocol     1 B | length      2 B  |              |
 *     | 0x0800 = IPv4    | 17 = UDP / 6=TCP | checksum    2 B  |              |
 *     +------------------+------------------+------------------+--------------+
 *
 *   - Ethernet 头固定 14 字节：目的 MAC(6) + 源 MAC(6) + EtherType(2, 大端)。
 *   - IPv4 头长度 = IHL * 4，通常 IHL=5，即 20 字节。
 *   - UDP 头固定 8 字节；UDP Length 字段包含 UDP 头 + payload。
 *   - payload 在帧中的偏移 = 14 + IHL*4 + 8。
 *   - 所有多字节头字段都是网络字节序（大端），必须转换后再参与计算。
 *   - 以太网最小帧 60 字节（不含 FCS），短包后面会有 0 填充；payload 长度
 *     必须用 UDP Length 计算，不能用整个帧长。
 *
 *   TCP 头（至少 20 字节）：
 *     - IPv4 protocol = 6；
 *     - TCP 头长度 = (tcp->data_off >> 4) * 4；
 *     - payload 偏移 = 14 + IHL*4 + TCP 头长度；
 *     - 关键字段：src/dst port、sent_seq、recv_ack、data_off、tcp_flags、
 *       rx_win、cksum；
 *     - TCP 是有状态的，必须维护连接状态和 seq/ack，不能像 UDP 一样
 *       无状态直接打印。
 *
 *   ICMP 头（8 字节）：
 *     - IPv4 protocol = 1；
 *     - 偏移 0：type(1) + code(1) + checksum(2)；
 *     - 偏移 4：identifier(2) + sequence(2)；
 *     - type=8 Echo Request，type=0 Echo Reply；
 *     - checksum 只覆盖 ICMP 头 + payload，不含 IP 伪首部；
 *     - payload 偏移 = 14 + IHL*4 + 8。
 *
 * ============================ 当前未实现的功能 ============================
 *   - 主动 ARP 请求（当前只做 ARP 应答 + 学习；主动请求/待发包队列未实现）
 *   - 其它 ICMP 类型（只实现 Echo Request/Reply；unreachable 等只打印）
 *   - ICMP 分片重组（ping -s 超过 MTU 时无法回复）
 *   - VLAN（0x8100 帧会被 EtherType 检查直接丢弃）
 *   - IPv4 分片重组（测试包请小于 MTU，payload <= 1472）
 *   - 完整 TCP：重传、拥塞控制、窗口缩放、SACK、时间戳、多连接、乱序重组
 *   - 接收方向的 IPv4/UDP/TCP 校验和验证
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_arp.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_byteorder.h>

#include <arpa/inet.h>

/* mbuf 池中的 mbuf 数量。512 个 RX 描述符 + 发送端突发流量足够用；
 * 如果以后做多队列或高吞吐，需要相应增大。 */
#define NUM_MBUFS 2048

/* 每次 rte_eth_rx_burst() 最多取多少个包。不能超过 RX 描述符数量太多，
 * 128 是常见的批处理大小。 */
#define BURST_SIZE 128

/* 本机 IPv4 地址（主机字节序的宏；gLocalIp 会转成网络字节序）。
 * 发送端测试的目标 IP 必须与这里一致，否则 ARP 不会应答。 */
#define USTACK_IPV4_ADDR RTE_IPV4(10, 10, 10, 100)

/* ARP 学习表：保存“IP -> MAC”的映射。
 * 当前主要用来记录请求者/应答者；回显本身直接用收到帧里的 MAC。
 * 查表函数留给以后实现主动 ARP 请求/待发包队列时使用。 */
#define ARP_TABLE_SIZE 16

struct arp_entry {
  uint32_t ip;                    /* 网络字节序 */
  struct rte_ether_addr mac;
  uint8_t vaild;                  /* 历史拼写，等价于 valid */
};

static struct arp_entry gArpTable[ARP_TABLE_SIZE];
static struct rte_ether_addr gLocalAddr;   /* 本机 DPDK 网卡 MAC */
static uint32_t gLocalIp;                  /* 本机 IP，网络字节序 */

/* TCP 标志位（对应 struct rte_tcp_hdr.tcp_flags） */
#define TCP_FLAG_FIN 0X01
#define TCP_FLAG_SYN 0X02
#define TCP_FLAG_RST 0X04
#define TCP_FLAG_PSH 0X08
#define TCP_FLAG_ACK 0X10

/* TCP 连接状态 */
#define TCP_STATE_FREE 0
#define TCP_STATE_SYN_RCVD 1
#define TCP_STATE_ESTABLISHED 2
#define TCP_STATE_FIN_WAIT 3

/* 回显分片大小：不解析对端 MSS，用保守的 536 字节。 */
#define TCP_ECHO_MSS 536

/* DPDK 端口号。只有一个 DPDK 网卡时通常就是 0。
 * 注意：这个编号是 EAL 探测到的端口序号，不是 Linux 的 ensXX 名字。 */
int gDpdkPortId = 0;

/* 端口配置。这里全部使用默认值：
 *   - mq_mode = 0：不使用 RSS 多队列；
 *   - mtu = 0：由 ethdev 自动设为 min(max_mtu, RTE_ETHER_MTU=1500)；
 *   - offloads = 0：不启用任何 RX/TX offload。
 * 早期版本写的是 .rxmode = { .mtu = RTE_ETHER_MTU }，该字段在 DPDK 24.11
 * 已不推荐显式设置，置 0 让 ethdev 使用默认值即可。 */
static const struct rte_eth_conf port_conf_default = {
  .rxmode = { .mtu = 0 }
};

/* 回显帧使用的地址和端口。
 * 这些值在收到包后由原始帧交换得到，全部保持网络字节序：
 *   gSrcMac / gSrcIpAddr / gSrcPort -> 回显帧的源（本 DPDK 网卡）
 *   gDstMac / gDstIpAddr / gDstPort -> 回显帧的目的（发送端）
 */
struct rte_ether_addr gSrcMac;
struct rte_ether_addr gDstMac;

uint32_t gSrcIpAddr;
uint32_t gDstIpAddr;

uint16_t gSrcPort;
uint16_t gDstPort;

/* 单个 TCP 连接的状态。
 * 实验只支持一个连接：新的 SYN 会覆盖旧连接。
 *
 * 字节序约定：
 *   - local_ip / remote_ip 存网络字节序（从收到的 IP 头原样拷贝），
 *     发送时直接赋值，不能再 htonl；
 *   - local_port / remote_port 存主机字节序，发送时 htons 一次。
 *
 * 注意：vaild 是历史拼写，功能上等价于 valid。
 */
struct tcp_conn {
  uint8_t vaild;                      /* 历史拼写，等价于 valid */
  uint8_t state;                      /* TCP_STATE_* */

  struct rte_ether_addr local_mac;    /* 本端（DPDK 网卡）MAC */
  struct rte_ether_addr remote_mac;   /* 对端（客户端）MAC */

  uint32_t local_ip;                  /* 网络字节序 */
  uint32_t remote_ip;                 /* 网络字节序 */

  uint16_t local_port;                /* 主机字节序 */
  uint16_t remote_port;               /* 主机字节序 */

  uint32_t iss;                       /* 本端初始发送序号 */
  uint32_t snd_nxt;                   /* 本端下一个发送序号 */
  uint32_t snd_una;                   /* 对端已确认的本端序号 */

  uint32_t irs;                       /* 对端初始发送序号 */
  uint32_t rcv_nxt;                   /* 本端期望的下一个接收序号 */
};

static struct tcp_conn gTcpConn;

/* 把 data 指向的 payload 封装成一个完整的 Ethernet + IPv4 + UDP 帧，
 * 写入 msg 指向的缓冲区。缓冲区由 uStack_send() 用
 * rte_pktmbuf_append() 准备好，长度至少为 total_length。
 *
 * 参数：
 *   msg          - 帧起始地址（指向 mbuf 的数据区）
 *   data         - 要回显的 payload（来自接收到的 UDP 包）
 *   total_length - 整个以太网帧长度 = 14 + 20 + 8 + payload_len
 *
 * 注意：所有多字节字段都按网络字节序写入；IPv4 首部校验和要在
 * UDP 头之前算好，UDP 校验和必须在 payload 拷贝完成之后再算。 */
static int uStack_encode_udp_pkt(uint8_t *msg, char* data, uint16_t total_length) {
  /* ---- Ethernet II 头：14 字节 ---- */
  struct rte_ether_hdr* ehdr = (struct rte_ether_hdr*)msg;
  ehdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  rte_memcpy(&ehdr->dst_addr, &gDstMac, sizeof(struct rte_ether_addr));
  rte_memcpy(&ehdr->src_addr, &gSrcMac, sizeof(struct rte_ether_addr));

  /* ---- IPv4 头：从以太网头之后（偏移 14）开始 ----
   * 不能写 msg + 1；那会覆盖以太网头的目的 MAC 和 EtherType。 */
  struct rte_ipv4_hdr* iphdr = (struct rte_ipv4_hdr*)(msg + sizeof(struct rte_ether_hdr));
  iphdr->version_ihl = 0x45;   /* 版本 4，IHL=5，即 20 字节 */
  iphdr->type_of_service = 0x0;
  iphdr->total_length = htons(total_length - sizeof(struct rte_ether_hdr));
  iphdr->packet_id = 0;
  iphdr->fragment_offset = 0;
  iphdr->time_to_live = 64;    /* 不能是 0；0 会被部分协议栈丢弃 */
  iphdr->next_proto_id = IPPROTO_UDP;
  iphdr->src_addr = gSrcIpAddr;
  iphdr->dst_addr = gDstIpAddr;

  /* IPv4 首部校验和：先把字段填好，清零校验和字段再计算。
   * rte_ipv4_cksum() 返回网络字节序，直接赋值。 */
  iphdr->hdr_checksum = 0;
  iphdr->hdr_checksum = rte_ipv4_cksum(iphdr);

  /* ---- UDP 头：紧跟 IPv4 头（iphdr + 1 即 20 字节后） ---- */
  struct rte_udp_hdr* udphdr = (struct rte_udp_hdr*)(iphdr + 1);
  udphdr->src_port = gSrcPort;
  udphdr->dst_port = gDstPort;

  uint16_t payload_len = total_length - sizeof(struct rte_ether_hdr)
                       - sizeof(struct rte_ipv4_hdr) - sizeof(struct rte_udp_hdr);

  /* 把接收到的 payload 真正复制到新帧的 UDP 头之后。
   * 不做这一步，回显出去的就是 mbuf 里的旧数据。 */
  rte_memcpy((char*)(udphdr + 1), data, payload_len);

  /* UDP Length 字段包含 UDP 头（8 字节）+ payload，不是只有 payload_len。 */
  udphdr->dgram_len = htons(payload_len + sizeof(struct rte_udp_hdr));

  /* UDP 校验和：必须在 UDP 头字段和 payload 都填好之后计算。
   * 先把校验和字段清零；返回值为网络字节序，直接赋值。 */
  udphdr->dgram_cksum = 0;
  udphdr->dgram_cksum = rte_ipv4_udptcp_cksum(iphdr, udphdr);

  return total_length;
}

/* 申请一个 TX mbuf，并用 uStack_encode_udp_pkt() 填充完整的回显帧。
 *
 * 这里用 rte_pktmbuf_append() 而不是手动给 data_len/pkt_len 赋值：
 *   - append 会检查 tailroom，并同时更新 data_len 和 pkt_len；
 *   - 返回的 pktdata 指向 mbuf 数据区，可直接交给编码函数。
 *
 * 注意：失败分支里释放的是 mbuf，不是 pktdata。 */
static struct rte_mbuf* uStack_send(struct rte_mempool* mbuf_pool, char *data, uint16_t data_len) {
  uint16_t total_length = data_len + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
                          + sizeof(struct rte_udp_hdr);

  struct rte_mbuf* mbuf = rte_pktmbuf_alloc(mbuf_pool);
  if(!mbuf) {
    rte_exit(EXIT_FAILURE, "Error with alloc mbuf in send\n");
  }

  uint8_t *pktdata = (uint8_t*)rte_pktmbuf_append(mbuf, total_length);
  if(!pktdata) {
    rte_pktmbuf_free(mbuf);   /* 释放的是 mbuf，不是 pktdata */
    rte_exit(EXIT_FAILURE, "No tailroom in send mbuf\n");
  }

  uStack_encode_udp_pkt(pktdata, data, total_length);

  return mbuf;
}

/* 构造并发送一个 TCP 段。
 *
 * 参数：
 *   mempool     - mbuf 池
 *   conn        - TCP 连接状态（提供 MAC/IP/端口）
 *   msg         - TCP payload（可为 NULL）
 *   payload_len - payload 长度
 *   seq / ack   - TCP 序号/确认号（主机字节序）
 *   flags       - TCP_FLAG_* 组合
 *
 * 成功返回 0；mbuf 分配失败或发送失败返回 -1。
 * TX 成功后 mbuf 归驱动所有，应用不能再 free。
 *
 * 注意：
 *   - conn->local_ip/remote_ip 已经是网络字节序，直接赋值；
 *   - conn->local_port/remote_port 是主机字节序，写入时 htons 一次；
 *   - TCP 校验和必须计算，结果为 0 时写 0xFFFF。
 */
static int tcp_send_segment(struct rte_mempool *mempool, struct tcp_conn *conn, const uint8_t* msg,
                            uint16_t payload_len, uint32_t seq, uint32_t ack, uint8_t flags) {
  const uint16_t tcp_hdr_len = sizeof(struct rte_tcp_hdr);
  const uint16_t ip_len = sizeof(struct rte_ipv4_hdr) + tcp_hdr_len + payload_len;
  const uint16_t frame_len = sizeof(struct rte_ether_hdr) + ip_len;

  struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
  if(!mbuf) {
    return -1;
  }

  uint8_t *pktdata = (uint8_t*)rte_pktmbuf_append(mbuf, frame_len);
  if(!pktdata) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }

  /* ---- Ethernet II 头：目的 = 客户端，源 = 本 DPDK 网卡 ---- */
  struct rte_ether_hdr *ehdr = (struct rte_ether_hdr*)pktdata;
  rte_memcpy(&ehdr->dst_addr, &conn->remote_mac, sizeof(struct rte_ether_addr));
  rte_memcpy(&ehdr->src_addr, &conn->local_mac, sizeof(struct rte_ether_addr));
  ehdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ---- IPv4 头 ----
   * conn->local_ip/remote_ip 已经是网络字节序，直接赋值，不能 htonl。 */
  struct rte_ipv4_hdr *iphdr = (struct rte_ipv4_hdr*)(pktdata + sizeof(struct rte_ether_hdr));
  iphdr->version_ihl = 0x45;
  iphdr->type_of_service = 0;
  iphdr->total_length = htons(ip_len);
  iphdr->packet_id = 0;
  iphdr->fragment_offset = 0;
  iphdr->time_to_live = 64;
  iphdr->next_proto_id = IPPROTO_TCP;
  iphdr->src_addr = conn->local_ip;
  iphdr->dst_addr = conn->remote_ip;

  iphdr->hdr_checksum = 0;
  iphdr->hdr_checksum = rte_ipv4_cksum(iphdr);

  /* ---- TCP 头（20 字节，data_off = 5） ----
   * conn->local_port/remote_port 是主机字节序，这里 htons 一次。 */
  struct rte_tcp_hdr *tcphdr = (struct rte_tcp_hdr*)((uint8_t*)iphdr + sizeof(struct rte_ipv4_hdr));
  tcphdr->src_port = htons(conn->local_port);
  tcphdr->dst_port = htons(conn->remote_port);
  tcphdr->sent_seq = htonl(seq);
  tcphdr->recv_ack = htonl(ack);
  tcphdr->data_off = (tcp_hdr_len / 4) << 4;   /* 5 << 4 = 0x50 */
  tcphdr->tcp_flags = flags;
  tcphdr->rx_win = htons(65535);
  tcphdr->cksum = 0;
  tcphdr->tcp_urp = 0;

  /* payload 紧跟在 TCP 头之后；注意指针要先转成 uint8_t* 再加偏移。 */
  if(payload_len != 0 && msg != NULL) {
    rte_memcpy((uint8_t*)tcphdr + tcp_hdr_len, msg, payload_len);
  }

  /* TCP 校验和必须计算；结果为 0 时写 0xFFFF（TCP 不允许 0）。 */
  uint16_t cksum = rte_ipv4_udptcp_cksum(iphdr, tcphdr);
  tcphdr->cksum = (cksum == 0) ? 0xFFFF : cksum;

  /* 发送成功后 mbuf 归驱动所有，只有失败时才由应用释放。 */
  uint16_t nb_tx = rte_eth_tx_burst(gDpdkPortId, 0, &mbuf, 1);
  if(nb_tx != 1) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }
  
  return 0;
}

/* 把 payload 按 TCP_ECHO_MSS 分片回显。
 * 每个分片都带 ACK|PSH；seq 从 conn->snd_nxt 开始递增。
 * 不解析对端 MSS，536 是保守值，保证不超过默认 MSS。 */
static void tcp_echo_payload(struct rte_mempool *mempool, struct tcp_conn *conn,
                             const uint8_t* payload, uint16_t payload_len) {
  uint16_t offset = 0;

  while(offset < payload_len) {
    uint16_t chunk = payload_len - offset;
    if(chunk > TCP_ECHO_MSS) {
      chunk = TCP_ECHO_MSS;
    }

    if(tcp_send_segment(mempool, conn, payload + offset, chunk,
                        conn->snd_nxt, conn->rcv_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH) < 0) {
      return;
    }

    conn->snd_nxt += chunk;
    offset += chunk;
  }
}

/* 处理一个 TCP 段：三次握手、数据回显、FIN/RST。
 *
 * 状态机：
 *   SYN                         -> SYN_RCVD，回 SYN|ACK
 *   ACK(ack == snd_nxt)         -> ESTABLISHED
 *   数据(seq == rcv_nxt)        -> 打印 + 回显
 *   FIN                         -> 回 FIN|ACK + 复位
 *
 * 只支持单连接、按序数据；不实现重传/乱序重组。
 * 注意：RX mbuf 由主循环统一释放，这里不 free。
 */
static void handle_tcp(struct rte_mempool *mempool,
                       struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr,
                       uint16_t ip_hdr_len, uint16_t pkt_len) {
  uint16_t l4_off = sizeof(struct rte_ether_hdr) + ip_hdr_len;

  if(l4_off + sizeof(struct rte_tcp_hdr) > pkt_len) {
    return;
  }

  struct rte_tcp_hdr *tcphdr = (struct rte_tcp_hdr*)((uint8_t*)iphdr + ip_hdr_len);

  /* TCP 头长度由 data offset 决定；客户端 SYN 通常带 MSS/SACK/时间戳选项，
   * tcp_hdr_len 可能是 24/32 字节，不能写死 20。 */
  uint16_t tcp_hdr_len = (tcphdr->data_off >> 4) * 4;
  if(l4_off + tcp_hdr_len > pkt_len) {
    return;
  }
  if(tcp_hdr_len < sizeof(struct rte_tcp_hdr)) {
    return;
  }
  
  uint16_t ip_len = rte_be_to_cpu_16(iphdr->total_length);
  if(ip_len < ip_hdr_len + tcp_hdr_len) {
    return;
  }
  
  uint16_t payload_len = ip_len - ip_hdr_len - tcp_hdr_len;
  if(payload_len + l4_off + tcp_hdr_len > pkt_len) {
    payload_len = pkt_len - l4_off - tcp_hdr_len;
  }

  /* payload 指针 = TCP 头起始 + 头长度。
   * 必须先转成 uint8_t*，否则 tcphdr + tcp_hdr_len 会按结构体大小放大 20 倍。 */
  uint8_t *payload = (uint8_t*)tcphdr + tcp_hdr_len;

  uint16_t flags = tcphdr->tcp_flags;
  uint32_t seq = rte_be_to_cpu_32(tcphdr->sent_seq);
  uint32_t ack = rte_be_to_cpu_32(tcphdr->recv_ack);
  uint16_t src_port = rte_be_to_cpu_16(tcphdr->src_port);
  uint16_t dst_port = rte_be_to_cpu_16(tcphdr->dst_port);

  /* ---------- SYN：新建连接或 SYN 重传 ---------- */
  if((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
    if(gTcpConn.vaild && gTcpConn.state == TCP_STATE_SYN_RCVD &&
       gTcpConn.local_ip == iphdr->dst_addr && gTcpConn.remote_ip == iphdr->src_addr &&
       gTcpConn.local_port == dst_port && gTcpConn.remote_port == src_port && gTcpConn.irs == seq) {
      tcp_send_segment(mempool, &gTcpConn, NULL, 0, gTcpConn.iss, gTcpConn.rcv_nxt,
                       TCP_FLAG_SYN | TCP_FLAG_ACK);
      return;
    }

    memset(&gTcpConn, 0, sizeof(gTcpConn));

    gTcpConn.vaild = 1;
    gTcpConn.state = TCP_STATE_SYN_RCVD;
    
    rte_memcpy(&gTcpConn.local_mac, &ehdr->dst_addr, sizeof(struct rte_ether_addr));
    rte_memcpy(&gTcpConn.remote_mac, &ehdr->src_addr, sizeof(struct rte_ether_addr));

    gTcpConn.local_ip = iphdr->dst_addr;
    gTcpConn.remote_ip = iphdr->src_addr;
    gTcpConn.local_port = dst_port;
    gTcpConn.remote_port = src_port;

    gTcpConn.irs = seq;
    gTcpConn.rcv_nxt = seq + 1;
    gTcpConn.iss = 0x12345678;
    gTcpConn.snd_nxt = gTcpConn.iss + 1;
    gTcpConn.snd_una = gTcpConn.iss;

    printf("TCP SYN from %u, new connection\n", src_port);
    tcp_send_segment(mempool, &gTcpConn, NULL, 0, gTcpConn.iss, gTcpConn.rcv_nxt,
                     TCP_FLAG_SYN | TCP_FLAG_ACK);
    
    return;
  }

  /* ---------- 其它包必须匹配当前连接 ---------- */
  if(!gTcpConn.vaild) {
    return;
  }

  if(gTcpConn.local_ip != iphdr->dst_addr || gTcpConn.remote_ip != iphdr->src_addr ||
     gTcpConn.local_port != dst_port || gTcpConn.remote_port != src_port) {
    return;
  }

  /* ---------- RST：立即复位 ---------- */
  if(flags & TCP_FLAG_RST) {
    printf("TCP RST, connection closed\n");
    memset(&gTcpConn, 0, sizeof(gTcpConn));
    return;
  }

  /* ---------- 三次握手第三步 ----------
   * 客户端发的是纯 ACK（没有 SYN），ack 必须等于
   * 我们 SYN-ACK 的 seq + 1，即 snd_nxt。 */
  if(gTcpConn.state == TCP_STATE_SYN_RCVD) {
    if(ack == gTcpConn.snd_nxt && (flags & TCP_FLAG_ACK)) {
      gTcpConn.state = TCP_STATE_ESTABLISHED;
      gTcpConn.snd_una = ack;
      printf("TCP connection established\n");
    }
    else {
      return;
    }
  }

  if(flags & TCP_FLAG_ACK) {
    if((int32_t)(ack - gTcpConn.snd_una) > 0) {
      gTcpConn.snd_una = ack;
    }
  }

  if(gTcpConn.state != TCP_STATE_ESTABLISHED) {
    return;
  }

  /* ---------- 数据：按序才打印/回显，乱序只回 ACK ---------- */
  if(payload_len > 0) {
    if(seq == gTcpConn.rcv_nxt) {
      printf("TCP length = %u, payload = ", payload_len);
      fwrite(payload, payload_len, 1, stdout);
      printf("\n");

      gTcpConn.rcv_nxt += payload_len;
      tcp_echo_payload(mempool, &gTcpConn, payload, payload_len);
    } else {
      tcp_send_segment(mempool, &gTcpConn, payload, payload_len,
                       gTcpConn.snd_nxt, gTcpConn.rcv_nxt, TCP_FLAG_ACK);
    }
  }

  /* ---------- FIN：回 FIN|ACK 并复位 ---------- */
  if(flags & TCP_FLAG_FIN) {
    uint32_t fin_seq = seq + payload_len;
    if(fin_seq == gTcpConn.rcv_nxt) {
      gTcpConn.rcv_nxt += 1;

      tcp_send_segment(mempool, &gTcpConn, NULL, 0, gTcpConn.snd_nxt, gTcpConn.rcv_nxt,
                       TCP_FLAG_FIN | TCP_FLAG_ACK);

      gTcpConn.snd_nxt += 1;
      printf("TCP FIN, connection closed\n");
      memset(&gTcpConn, 0, sizeof(gTcpConn));
    }
  }
}

/* 处理一个 UDP 段：打印 payload 并原样回显。
 *
 * UDP 是无状态的：直接把收到的源/目的地址交换后发回去，
 * 不需要握手、序号或连接状态。
 *
 * 注意：RX mbuf 由主循环统一释放，这里不 free。
 */
static void handle_udp(struct rte_mempool *mempool,
                       struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr,
                       uint16_t ip_hdr_len, uint16_t pkt_len) {
  /* UDP 头在帧中的起始偏移 = 14 + IHL*4 */
  uint32_t udp_off = sizeof(struct rte_ether_hdr) + ip_hdr_len;

  /* 防止畸形/截断包导致越界读：至少要能放下 UDP 头 */
  if(udp_off + sizeof(struct rte_udp_hdr) > pkt_len) {
    return;
  }

  /* ---- 11.4 定位 UDP 头 ---- */
  struct rte_udp_hdr *udphdr = (struct rte_udp_hdr*)((uint8_t*)iphdr + ip_hdr_len);
        
        /* ---- 11.5 读取 UDP 长度（关键：字节序转换） ----
         * dgram_len 包含 UDP 头(8 字节) + payload，且是网络字节序。
         * 小端机器上不转换会把 18 读成 4608。 */
  uint16_t length = rte_be_to_cpu_16(udphdr->dgram_len);
  if(length < sizeof(struct rte_udp_hdr)) {
    return;
  }

  /* ---- 11.6 准备回显地址：交换源和目的 ----
  * 收到的帧：源 = 发送端，目的 = 本 DPDK 网卡。
  * 回显帧要反过来：目的 = 发送端，源 = 本 DPDK 网卡。
  * 下面按原始字节拷贝，保留网络字节序，不需要 htons/ntohl。 */
  rte_memcpy(&gDstMac, &ehdr->src_addr, sizeof(struct rte_ether_addr));  /* 回显目的 MAC */
  rte_memcpy(&gSrcMac, &ehdr->dst_addr, sizeof(struct rte_ether_addr));  /* 回显源 MAC   */

  rte_memcpy(&gSrcIpAddr, &iphdr->dst_addr, sizeof(uint32_t));  /* 回显源 IP */
  rte_memcpy(&gDstIpAddr, &iphdr->src_addr, sizeof(uint32_t));  /* 回显目的 IP */

  rte_memcpy(&gSrcPort, &udphdr->dst_port, sizeof(uint16_t));  /* 回显源端口 = 9999 */
  rte_memcpy(&gDstPort, &udphdr->src_port, sizeof(uint16_t));  /* 回显目的端口 = 发送端源端口 */

  /* ---- 11.7 payload 长度 = UDP 总长度 - UDP 头长度 ---- */
  uint16_t data_length = length - sizeof(struct rte_udp_hdr);

  /* ---- 11.8 用 mbuf 实际长度做上限保护 ----
  * 以太网短帧会被填充到 60 字节；如果 dgram_len 与实际数据不一致
  * （畸形包、截断包），这里保证 fwrite 不会读越界。
  * 此时 pkt_len >= udp_off + 8 已在 11.3 检查过，不会无符号下溢。 */
  uint32_t avail = pkt_len - sizeof(struct rte_udp_hdr) - udp_off;
  if(data_length > avail) {
    data_length = avail;
  }

  /* ---- 11.9 输出 ----
  * 用 fwrite 按精确长度输出，而不是 "%.*s"：
  *   - payload 不保证以 '\0' 结尾；
  *   - payload 可能包含 0 字节；
  *   - 长度字段出错时 %.*s 还可能越界读。
  * payload 紧跟在 UDP 头之后：udphdr + 1。 */
  printf("length = %u, data_length = %u, payload = ", length, data_length);
  fwrite((char*)(udphdr + 1), 1, data_length, stdout);
  printf("\n");

  /* ---- 11.10 回显：构造并发送 UDP 包 ----
  * uStack_send() 内部会分配 TX mbuf、构造完整帧并拷贝 payload。
  * rte_eth_tx_burst() 成功后 mbuf 归驱动所有，不能再 free；
  * 只有返回 0（队列满/发送失败）时才由应用释放。 */
  struct rte_mbuf *txmbuf = uStack_send(mempool, (char*)(udphdr + 1), data_length);

  if(txmbuf) {
    uint16_t nb_tx = rte_eth_tx_burst(gDpdkPortId, 0, &txmbuf, 1);

    if(nb_tx != 1) {
      rte_pktmbuf_free(txmbuf);
    }
  }
}

/* 计算 ICMP 校验和。
 *
 * ICMP 校验和只覆盖 ICMP 头 + payload，不包含 IP 伪首部；
 * 这一点和 UDP/TCP 不同（UDP/TCP 用 rte_ipv4_udptcp_cksum 加伪首部）。
 *
 * rte_raw_cksum() 返回 32 位和，折叠两次后取反。
 * 结果可以直接写入 struct rte_icmp_hdr.icmp_cksum（网络字节序字段），
 * 与 rte_ipv4_cksum() 的用法一致。 */
static uint16_t icmp_cksum(const void *data, uint16_t len) {
  uint32_t sum = rte_raw_cksum(data, len);

  sum = (sum & 0xFFFF) + (sum >> 16);
  sum = (sum & 0xFFFF) + (sum >> 16);

  return (uint16_t)~sum;
}

/* 构造并发送 ICMP Echo Reply。
 *
 * 参数：
 *   mempool  - mbuf 池
 *   ehdr     - 收到的 Ethernet 头（提供源/目的 MAC）
 *   iphdr    - 收到的 IPv4 头（提供源/目的 IP）
 *   icmphdr  - 收到的 ICMP 头（type=8）
 *   icmp_len - ICMP 消息长度 = ICMP 头 + payload
 *
 * 回显帧：
 *   Ethernet: dst = 请求源 MAC，src = 请求目的 MAC（本 DPDK 网卡）
 *   IPv4    : src = 请求目的 IP（10.10.10.100），dst = 请求源 IP
 *             protocol = 1 (ICMP)，TTL = 64
 *   ICMP    : type = 0 (Echo Reply)，code = 0
 *             ident / seq 原样拷贝，payload 原样拷贝
 *
 * 成功返回 0；mbuf 分配失败或发送失败返回 -1。
 * TX 成功后 mbuf 归驱动所有，应用不能再 free。 */
static int icmp_send_reply(struct rte_mempool *mempool, struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr,
                           struct rte_icmp_hdr *icmphdr,
                           uint16_t icmp_len) {
  const uint16_t ip_len = icmp_len + sizeof(struct rte_ipv4_hdr);
  const uint16_t frame_len = ip_len + sizeof(struct rte_ether_hdr);

  struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
  if(!mbuf) {
    return -1;
  }

  uint8_t *pkt = (uint8_t*)rte_pktmbuf_append(mbuf, frame_len);
  if(!pkt) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }

  /* ---- Ethernet II：交换 MAC ---- */
  struct rte_ether_hdr *out_ehdr = (struct rte_ether_hdr*)pkt;
  rte_memcpy(&out_ehdr->dst_addr, &ehdr->src_addr, sizeof(struct rte_ether_addr));
  rte_memcpy(&out_ehdr->src_addr, &ehdr->dst_addr, sizeof(struct rte_ether_addr));
  out_ehdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ---- IPv4：交换 IP，protocol = ICMP ---- */
  struct rte_ipv4_hdr *out_iphdr = (struct rte_ipv4_hdr*)(pkt + sizeof(struct rte_ether_hdr));
  out_iphdr->version_ihl = 0x45;
  out_iphdr->type_of_service = 0;
  out_iphdr->total_length = htons(ip_len);
  out_iphdr->packet_id = 0;
  out_iphdr->fragment_offset = 0;
  out_iphdr->time_to_live = 64;
  out_iphdr->next_proto_id = IPPROTO_ICMP;
  out_iphdr->src_addr = iphdr->dst_addr;
  out_iphdr->dst_addr = iphdr->src_addr;

  /* IPv4 首部校验和：字段填好后清零再计算，返回值直接写入 */
  out_iphdr->hdr_checksum = 0;
  out_iphdr->hdr_checksum = rte_ipv4_cksum(out_iphdr);

  /* ---- ICMP Echo Reply ----
   * ident/seq 是网络字节序，直接按字段拷贝，不要再 htons。 */
  struct rte_icmp_hdr *out_icmphdr = (struct rte_icmp_hdr*)((uint8_t*)out_iphdr + sizeof(struct rte_ipv4_hdr));
  out_icmphdr->icmp_type = RTE_ICMP_TYPE_ECHO_REPLY;
  out_icmphdr->icmp_code = 0;
  out_icmphdr->icmp_cksum = 0;
  out_icmphdr->icmp_ident = icmphdr->icmp_ident;
  out_icmphdr->icmp_seq_nb = icmphdr->icmp_seq_nb;

  /* payload 紧跟在 ICMP 头之后。
   * 注意：源必须是“请求”的 ICMP payload，即 icmphdr + 8；
   * 不能写 out_iphdr/out_icmphdr，那是刚分配的发送缓冲区。 */
  const uint16_t payload_len = icmp_len - sizeof(struct rte_icmp_hdr);
  rte_memcpy((uint8_t*)out_icmphdr + sizeof(struct rte_icmp_hdr),
             (const uint8_t*)icmphdr + sizeof(struct rte_icmp_hdr), payload_len);

  /* ICMP 校验和：必须在 type/code/ident/seq/payload 都填好后计算。 */
  out_icmphdr->icmp_cksum = icmp_cksum(out_icmphdr, icmp_len);

  /* 发送成功后 mbuf 归驱动所有，只有失败时才由应用释放。 */
  uint16_t nb_tx = rte_eth_tx_burst(gDpdkPortId, 0, &mbuf, 1);
  if(nb_tx != 1) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }

  return 0;
}

/* 处理一个 ICMP 报文：目前只回复 Echo Request（ping）。
 *
 * 长度计算：
 *   ip_len   = IPv4 total_length（不含以太网头）
 *   icmp_len = ip_len - IHL*4
 * 不能用 pkt_len：短帧会被以太网填充到 60 字节，填充不属于
 * ICMP payload，也不能算进 ICMP 校验和。
 *
 * 分片包直接忽略：ping 默认 56 字节 payload 不会分片；
 * ping -s 超过 MTU 时会分片，当前不重组。
 *
 * 注意：RX mbuf 由主循环统一释放，这里不 free。 */
static void handle_icmp(struct rte_mempool *mempool, struct rte_ether_hdr *ehdr, struct rte_ipv4_hdr *iphdr,
                        uint16_t ip_hdr_len, uint16_t pkt_len) {
  uint32_t icmp_off = sizeof(struct rte_ether_hdr) + ip_hdr_len;

  /* 至少要能放下 ICMP 头（8 字节） */
  if(icmp_off + sizeof(struct rte_icmp_hdr) > pkt_len) {
    return;
  }

  /* 分片包（MF=1 或 offset != 0）不处理，避免把非首片数据当成 ICMP 头 */
  uint16_t flag = rte_be_to_cpu_16(iphdr->fragment_offset);
  if(flag & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) {
    return;
  }

  uint16_t ip_len = rte_be_to_cpu_16(iphdr->total_length);
  if(ip_len < ip_hdr_len + sizeof(struct rte_icmp_hdr)) {
    return;
  }

  /* 完整帧长度 = 14 + ip_len；pkt_len 可能包含以太网填充，用 >= 判断 */
  if(sizeof(struct rte_ether_hdr) + ip_len > pkt_len) {
    return;
  }

  uint16_t icmp_len = ip_len - ip_hdr_len;

  /* 关键：iphdr 是 struct rte_ipv4_hdr *，必须先转 uint8_t* 再加字节偏移。
   * 写成 iphdr + ip_hdr_len 会前进 ip_hdr_len * sizeof(struct rte_ipv4_hdr)
   * = 400 字节，读到清零内存，icmp_type 变成 0（Echo Reply），不会回包。 */
  struct rte_icmp_hdr *icmphdr = (struct rte_icmp_hdr*)((uint8_t*)iphdr + ip_hdr_len);

  if(icmphdr->icmp_type == RTE_ICMP_TYPE_ECHO_REQUEST && icmphdr->icmp_code == 0) {
    printf("ICMP echo request id=%u seq=%u len=%u, replying\n",
           rte_be_to_cpu_16(icmphdr->icmp_ident),
           rte_be_to_cpu_16(icmphdr->icmp_seq_nb),
           icmp_len);

    if(icmp_send_reply(mempool, ehdr, iphdr, icmphdr, icmp_len) != 0) {
      printf("ICMP echo reply send failed\n");
    }
  } else if(icmphdr->icmp_type == RTE_ICMP_TYPE_ECHO_REPLY) {
    /* 收到对端回给我们的 Echo Reply（本程序不主动 ping，只记录） */
    printf("ICMP echo reply id=%u seq=%u len=%u\n",
           rte_be_to_cpu_16(icmphdr->icmp_ident),
           rte_be_to_cpu_16(icmphdr->icmp_seq_nb),
           icmp_len);
  } else {
    /* 其它 ICMP 类型：只打印，不回复 */
    printf("ICMP type=%u code=%u len=%u (ignored)\n",
           icmphdr->icmp_type, icmphdr->icmp_code, icmp_len);
  }
}

/* 发送 ARP Reply。
 *
 * ARP Reply 的以太网头：
 *   目的 MAC = 请求者 MAC（req->arp_sha）
 *   源   MAC = 本机 MAC（gLocalAddr）
 *   EtherType = 0x0806
 *
 * ARP 载荷：
 *   sender MAC/IP = 本机；target MAC/IP = 请求者。
 *
 * ARP 帧本身只有 42 字节，以太网最小帧 60 字节；统一补零到 60。
 * TX 成功后 mbuf 归驱动所有，应用不能再 free。
 * 返回 0 成功，-1 失败。
 */
static int arp_send_reply(struct rte_mempool *mempool, const struct rte_arp_ipv4 *req) {
  const uint16_t frame_len = 60;

  struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
  if(!mbuf) {
    return -1;
  }

  /* rte_pktmbuf_append() 会设置 data_len/pkt_len 并检查 tailroom */
  uint8_t *pkt = (uint8_t*)rte_pktmbuf_append(mbuf, frame_len);
  if(!pkt) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }
  memset(pkt, 0, frame_len);

  /* ---- Ethernet II 头：目的 = 请求者，源 = 本机 ---- */
  struct rte_ether_hdr *ehdr = (struct rte_ether_hdr*)pkt;
  rte_memcpy(&ehdr->dst_addr, &req->arp_sha, sizeof(struct rte_ether_addr));
  rte_memcpy(&ehdr->src_addr, &gLocalAddr, sizeof(struct rte_ether_addr));
  ehdr->ether_type = htons(RTE_ETHER_TYPE_ARP);

  /* ---- ARP 头 ---- */
  struct rte_arp_hdr *arphdr = (struct rte_arp_hdr*)(pkt + sizeof(struct rte_ether_hdr));
  arphdr->arp_hardware = htons(RTE_ARP_HRD_ETHER);       /* 1 = Ethernet */
  arphdr->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);     /* 0x0800 */
  arphdr->arp_hlen = sizeof(struct rte_ether_addr);      /* 6 */
  arphdr->arp_plen = sizeof(uint32_t);                   /* 4 */
  arphdr->arp_opcode = htons(RTE_ARP_OP_REPLY);          /* 2 = Reply */

  rte_memcpy(&arphdr->arp_data.arp_sha, &gLocalAddr, sizeof(struct rte_ether_addr));
  arphdr->arp_data.arp_sip = gLocalIp;
  rte_memcpy(&arphdr->arp_data.arp_tha, &req->arp_sha, sizeof(struct rte_ether_addr));
  arphdr->arp_data.arp_tip = req->arp_sip;

  uint16_t nb_tx = rte_eth_tx_burst(gDpdkPortId, 0, &mbuf, 1);
  if(nb_tx != 1) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }

  return 0;
}

/* 更新 ARP 学习表：IP 已存在则更新 MAC，否则写入空槽位。
 * 注意：mac 已经是指针，rte_memcpy 的源要传 mac，不能传 &mac。
 * vaild 是历史拼写，等价于 valid。 */
static void arp_table_update(uint32_t ip, struct rte_ether_addr *mac) {
  int free_ip = -1;

  for(int i = 0; i < ARP_TABLE_SIZE; ++i) {
    if(gArpTable[i].vaild && gArpTable[i].ip == ip) {
      rte_memcpy(&gArpTable[i].mac, mac, sizeof(struct rte_ether_addr));
      return;
    }

    if(!gArpTable[i].vaild && free_ip < 0) {
      free_ip = i;
    }
  }

  if(free_ip >= 0) {
    gArpTable[free_ip].ip = ip;
    rte_memcpy(&gArpTable[free_ip].mac, mac, sizeof(struct rte_ether_addr));
    gArpTable[free_ip].vaild = true;
  }
}

/* 查询 ARP 表：返回 MAC 指针，找不到返回 NULL。
 * 当前回显直接使用收到帧里的 MAC，这个函数预留给以后实现
 * “主动 ARP 请求 / 待发包队列”时使用。 */
static const struct rte_ether_addr* arp_table_lookup(uint32_t ip) {
  for(int i = 0; i < ARP_TABLE_SIZE; ++i) {
    if(gArpTable[i].vaild && gArpTable[i].ip == ip) {
      return &gArpTable[i].mac;
    }
  }

  return NULL;
}

/* 处理一个 ARP 帧。
 *
 * 请求（opcode=1）：
 *   - 先学习请求者的 IP/MAC；
 *   - target IP == gLocalIp 时回 ARP Reply；
 *   - target IP 不是本机时只学习，不应答。
 *
 * 应答（opcode=2）：
 *   - 学习对端 IP/MAC（预留给主动 ARP 请求）。
 *
 * 注意：RX mbuf 由主循环统一释放，这里不 free。
 */
static void handle_arp(struct rte_mempool *mempool, struct rte_ether_hdr* ehdr, uint32_t pkt_len) {
  /* ARP 帧至少 14 + 28 = 42 字节 */
  if(pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr)) {
    return;
  }

  struct rte_arp_hdr *arphdr = (struct rte_arp_hdr*)((uint8_t*)ehdr + sizeof(struct rte_ether_hdr));

  /* 只处理 Ethernet + IPv4 的 ARP */
  if(arphdr->arp_hardware != rte_be_to_cpu_16(RTE_ARP_HRD_ETHER)
     || arphdr->arp_protocol != rte_be_to_cpu_16(RTE_ETHER_TYPE_IPV4)
     || arphdr->arp_hlen != sizeof(struct rte_ether_addr)
     || arphdr->arp_plen != sizeof(uint32_t)) {
    return;
  }

  uint16_t opcode = rte_be_to_cpu_16(arphdr->arp_opcode);

  if(opcode == RTE_ARP_OP_REQUEST) {
    /* 学习请求者的 IP/MAC */
    arp_table_update(arphdr->arp_data.arp_sip, &arphdr->arp_data.arp_sha);

    /* 只应答“谁有本机 IP”的请求 */
    if(arphdr->arp_data.arp_tip != gLocalIp) {
      return;
    }

    /* arp_sip 是网络字节序，用 inet_ntoa 按点分十进制打印。
     * 不能直接 fwrite 四个字节：10.10.10.10 里有 0x0A（换行）。 */
    struct in_addr sip = { .s_addr = arphdr->arp_data.arp_sip };
    printf("ARP request from %s, replying\n", inet_ntoa(sip));

    if(arp_send_reply(mempool, &arphdr->arp_data) != 0) {
      printf("ARP reply send failed\n");
    }
  } else if(opcode == RTE_ARP_OP_REPLY) {
    /* 学习应答者（预留给主动 ARP 请求） */
    arp_table_update(arphdr->arp_data.arp_sip, &arphdr->arp_data.arp_sha);
  }
}

int main(int argc, char *argv[]) {
    /* ------------------------------------------------------------------
     * 1. 初始化 EAL（Environment Abstraction Layer）
     *    - 解析 --iova-mode=pa、-l、--file-prefix 等参数；
     *    - 初始化大页内存、PCI 总线、vfio/uio 驱动；
     *    - 探测并注册 DPDK 网卡。
     *    必须在任何其他 DPDK API 之前调用。
     * ------------------------------------------------------------------ */
    if(rte_eal_init(argc, argv) < 0) {
      rte_exit(EXIT_FAILURE, "Error with EAL init\n");
    }

    /* ------------------------------------------------------------------
     * 2. 确认至少有一个可用的 DPDK 以太网端口
     *    rte_eth_dev_count_avail() 返回 uint16_t，所以不能用 < 0 判断。
     * ------------------------------------------------------------------ */
    uint16_t nb_sys_ports = rte_eth_dev_count_avail();
    if(nb_sys_ports == 0) {
      rte_exit(EXIT_FAILURE, "No support eth found\n");
    }
    printf("nb_sys_ports = %d\n", nb_sys_ports);

    /* ------------------------------------------------------------------
     * 3. 读取端口信息（本例只用来确认端口存在；dev_info 暂未使用）
     * ------------------------------------------------------------------ */
    struct rte_eth_dev_info dev_info;
    if(rte_eth_dev_info_get(gDpdkPortId, &dev_info) < 0) {
      rte_exit(EXIT_FAILURE, "Error with dev_info get\n");
    }

    /* ------------------------------------------------------------------
     * 4. 创建 mbuf 内存池
     *    - mbuf 是 DPDK 收发数据的基本单位，包含头部和一块数据缓冲区；
     *    - 2048 个 mbuf，cache_size=0，priv_size=0；
     *    - RTE_MBUF_DEFAULT_BUF_SIZE = 2048 + 128(headroom)，足够 1500 MTU；
     *    - rte_socket_id() 取当前 lcore 的 NUMA socket；单 NUMA 虚拟机为 0。
     *    注意：mbuf 池必须足够大，否则 RX 描述符无法全部填充，表现为 RX-nombuf。
     * ------------------------------------------------------------------ */
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("mbuftool", NUM_MBUFS, 0, 0,
                                                            RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if(!mbuf_pool) {
      rte_exit(EXIT_FAILURE, "Could not create mbuftool\n");
    }

    /* ------------------------------------------------------------------
     * 5. 配置端口：1 个 RX 队列 + 1 个 TX 队列
     *    RX 用于收包；TX 用于回显。即使某次没有回显，TX 队列也必须配置，
     *    因为 rte_eth_dev_start() 要求已声明的队列都完成 setup。
     * ------------------------------------------------------------------ */
    const int nums_rx_queues = 1;
    const int nums_tx_queues = 1;
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    port_conf = port_conf_default;
    if(rte_eth_dev_configure(gDpdkPortId, nums_rx_queues, nums_tx_queues, &port_conf) < 0) {
      rte_exit(EXIT_FAILURE, "Could not configure eth\n");
    }

    /* ------------------------------------------------------------------
     * 6. 设置 RX 队列 0
     *    - 512 个描述符：vmxnet3 的下限是 128，但 128 正好在边界上，
     *      源码里的错误信息甚至写着 Min: 256；512 与 testpmd 的默认行为更接近；
     *    - socket_id 取设备所在 socket；
     *    - rx_conf 传 NULL，表示使用 ethdev 的默认 RX 配置；
     *    - mbuf_pool 是该队列取包/放包使用的内存池。
     * ------------------------------------------------------------------ */
    if(rte_eth_rx_queue_setup(gDpdkPortId, 0, 512,
                           rte_eth_dev_socket_id(gDpdkPortId), NULL, mbuf_pool) < 0) {
      rte_exit(EXIT_FAILURE, "Could not set rx_queues\n");
    }

    /* ------------------------------------------------------------------
     * 7. 设置 TX 队列 0
     *    回显时通过这个队列调用 rte_eth_tx_burst() 发包。
     *    rte_eth_dev_start() 要求所有已声明数量的队列都完成 setup。
     * ------------------------------------------------------------------ */
    if(rte_eth_tx_queue_setup(gDpdkPortId, 0, 512, rte_eth_dev_socket_id(gDpdkPortId), NULL) < 0) {
      rte_exit(EXIT_FAILURE, "Could not set tx_queues\n");
    }

    /* ------------------------------------------------------------------
     * 8. 启动端口
     *    - 使能 RX/TX 队列、完成设备激活；
     *    - 之后就可以调用 rte_eth_rx_burst() 轮询收包；
     *    - 成功不代表链路一定 up，链路状态可以用 rte_eth_link_get() 查询。
     * ------------------------------------------------------------------ */
    if(rte_eth_dev_start(gDpdkPortId) < 0) {
      rte_exit(EXIT_FAILURE, "Could not start\n");
    }

    /* ------------------------------------------------------------------
     * 9. 开启混杂模式
     *    - 让网卡接收目的 MAC 不是自己的帧（方便调试）；
     *    - 本例的目的 MAC 本来就是本网卡，理论上不开也能收；
     *    - 某些 vmxnet3/虚拟交换机组合下，混杂模式与单播过滤可能互相影响，
     *      若出现“广播能收、单播收不到”，可以尝试把这行注释掉再测。
     * ------------------------------------------------------------------ */
    rte_eth_promiscuous_enable(gDpdkPortId);

    /* ------------------------------------------------------------------
     * 10. 打印本端口 MAC，并保存 ARP 需要的本机 MAC/IP
     *     - gLocalAddr 是 ARP 应答的源 MAC；
     *     - gLocalIp 是 ARP 应答的本机 IP，必须是网络字节序；
     *     - MAC 是 VMware 分配给虚拟网卡的，换虚拟机/换网卡后会变化，
     *       不要硬编码旧值。
     * ------------------------------------------------------------------ */
    struct rte_ether_addr addr;
    rte_eth_macaddr_get(gDpdkPortId, &addr);
    printf("Port %d MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
       gDpdkPortId,
       addr.addr_bytes[0], addr.addr_bytes[1], addr.addr_bytes[2],
       addr.addr_bytes[3], addr.addr_bytes[4], addr.addr_bytes[5]);

    rte_memcpy(&gLocalAddr, &addr, sizeof(struct rte_ether_addr));
    gLocalIp = rte_cpu_to_be_32(USTACK_IPV4_ADDR);
    printf("Ustack IPv4: 10.10.10.100\n");
    
    printf("start eth success\n");

    /* ------------------------------------------------------------------
     * 11. 主循环：轮询收包（Poll Mode Driver，没有中断）
     *     rte_eth_rx_burst() 每次最多取 BURST_SIZE 个 mbuf，返回实际数量；
     *     没有包时返回 0，循环会立刻再次轮询，因此这个线程会 100% 占用一个核。
     * ------------------------------------------------------------------ */
    while(1) {
      struct rte_mbuf *mbufs[BURST_SIZE];
      uint16_t nb_recvd = rte_eth_rx_burst(gDpdkPortId, 0, mbufs, BURST_SIZE);

      /* rte_eth_rx_burst 的返回值不可能大于 BURST_SIZE，这里的检查只是兜底 */
      if(nb_recvd > BURST_SIZE) {
        rte_exit(EXIT_FAILURE, "Could not set rx burst\n");
      }

      /* 逐个处理本批次收到的包 */
      for(uint16_t i = 0; i < nb_recvd; ++i) {
        /* 关键：取当前 mbuf 的实际数据长度。
         * 千万不要在这里再调用一次 rte_eth_rx_burst——那会把“包数量”
         * 当成“包长度”，并且覆盖 mbufs[] 数组，导致所有包被丢弃。
         * 单段 mbuf 用 data_len 即可；如果以后开启 scatter/jumbo，
         * 应改用 rte_pktmbuf_pkt_len()。 */
        uint32_t pkt_len = rte_pktmbuf_data_len(mbufs[i]);

        /* 长度不足以容纳 Ethernet 头 + 最小 IPv4 头，直接丢弃 */
        if(pkt_len < sizeof(struct rte_ether_hdr)) {
          rte_pktmbuf_free(mbufs[i]);
          continue;
        }

        /* ---- 11.1 解析 Ethernet II 头（前 14 字节） ----
         * EtherType 在帧里是大端；0x0800 表示 IPv4，0x0806 是 ARP，
         * 0x86DD 是 IPv6，0x8100 是 VLAN Tag。
         * ARP 在上面的分支单独处理；这里只处理 IPv4，
         * IPv6/VLAN 等其它类型丢弃。 */
        struct rte_ether_hdr *ehdr = rte_pktmbuf_mtod(mbufs[i], struct rte_ether_hdr*);

        /* ---- 11.2 ARP：必须在 IPv4 过滤之前处理 ----
         * ARP 的 EtherType 是 0x0806；handle_arp() 内部负责：
         *   - 学习请求者/应答者的 IP-MAC；
         *   - 对“谁有本机 IP”的请求回 ARP Reply。 */
        if(ehdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
          handle_arp(mbuf_pool, ehdr, pkt_len);
          rte_pktmbuf_free(mbufs[i]);
          continue;
        }

        if(ehdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {          
          rte_pktmbuf_free(mbufs[i]);
          continue;
        }
        
        if(pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
          rte_pktmbuf_free(mbufs[i]);
          continue;
        }
        
        /* ---- 11.2 定位 IPv4 头 ----
         * 以太网头固定 14 字节，所以 IPv4 头从帧偏移 14 开始。
         * rte_pktmbuf_mtod_offset() 返回的是 mbuf 数据区内的指针。 */
        struct rte_ipv4_hdr *iphdr = rte_pktmbuf_mtod_offset(mbufs[i], struct rte_ipv4_hdr*,
                                                             sizeof(struct rte_ether_hdr));

        /* ---- 11.3 计算 IPv4 头长度 ----
         * version_ihl 的高 4 位是版本，低 4 位是 IHL（单位是 4 字节）。
         * 标准头 IHL=5 -> 20 字节；带选项时可能更大。
         * 不能写死 iphdr + 1。 */
        uint16_t ihl = (iphdr->version_ihl & RTE_IPV4_HDR_IHL_MASK) * RTE_IPV4_IHL_MULTIPLIER;
        if(ihl < sizeof(struct rte_ipv4_hdr) || ihl + sizeof(struct rte_ether_hdr) > pkt_len) {
          rte_pktmbuf_free(mbufs[i]);
          continue;
        }

        /* 按协议号分发：TCP(6) / UDP(17) / ICMP(1)；其它协议忽略。
         * handler 内部不释放 RX mbuf，统一在分发之后释放。 */
        if(iphdr->next_proto_id == IPPROTO_TCP) {
          handle_tcp(mbuf_pool, ehdr, iphdr, ihl, pkt_len);
        } else if(iphdr->next_proto_id == IPPROTO_UDP) {
          handle_udp(mbuf_pool, ehdr, iphdr, ihl, pkt_len);
        } else if(iphdr->next_proto_id == IPPROTO_ICMP) {
          handle_icmp(mbuf_pool, ehdr, iphdr, ihl, pkt_len);
        }
        
        /* ---- 11.11 释放 RX mbuf ----
         * 每个从 RX 队列取出的 mbuf 处理完必须归还内存池，
         * 否则池子耗尽后 RX 描述符无法补充，表现为 RX-nombuf / 收不到新包。 */
        rte_pktmbuf_free(mbufs[i]);
      }
    }

    return 0;
}
