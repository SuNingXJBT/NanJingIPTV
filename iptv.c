/*
* IPTV DHCP Client for Windows
* 功能：获取IPTV专用IP地址，配置网卡，管理DHCP租约
*
* 编译：cl iptv.c /O2 /W3 /Fe:iptv.exe
* 运行：需要管理员权限
回放地址样例
http://gslb.itv.jsinfo.net:6060/00000002/01000000000000000000000209123269/index.m3u8?Playtype=1&Playseek=20260522170000-20260522183000
http://gslb.itv.jsinfo.net:6060/00000002/01000000000000000000000210913770/index.m3u8?Playtype=1&Playseek=20260524150600-20260524153000
*/

//#include "public.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <conio.h>
#include <time.h>
#include <netioapi.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

// ============================================================================
// 常量定义
// ============================================================================

// DHCP 端口
#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68

// DHCP 消息类型
#define BOOTREQUEST        1
#define BOOTREPLY          2

// DHCP 消息类型值
#define DHCPDISCOVER       1
#define DHCPOFFER          2
#define DHCPREQUEST        3
#define DHCPDECLINE        4
#define DHCPACK            5
#define DHCPNAK            6
#define DHCPRELEASE        7

// DHCP 选项代码
#define OPTION_SUBNET_MASK      1
#define OPTION_ROUTER           3
#define OPTION_DNS              6
#define OPTION_DOMAIN_NAME      15
#define OPTION_LEASE_TIME       51
#define OPTION_MSG_TYPE         53
#define OPTION_SERVER_ID        54
#define OPTION_PARAM_REQ_LIST   55
#define OPTION_MAXSIZE          57
#define OPTION_RENEWAL_T1       58
#define OPTION_REBINDING_T2     59
#define OPTION_VENDOR_ID        60
#define OPTION_CLIENT_ID        61
#define OPTION_VENDOR_SPECIFIC  43
#define OPTION_REQUESTED_IP     50
#define OPTION_END              255

// DHCP Cookie
#define DHCP_COOKIE             0x63825363

// 超时设置（秒）
#define DISCOVER_TIMEOUT        5
#define RENEW_TIMEOUT           5
#define REBIND_RETRY_INTERVAL   2

// 最大重试次数
#define MAX_DHCP_RETRIES        3

// ============================================================================
// 数据结构定义
// ============================================================================

#pragma pack(push, 1)
typedef struct {
	uint8_t  op;            // 消息类型: 1=请求, 2=响应
	uint8_t  htype;         // 硬件地址类型: 1=Ethernet
	uint8_t  hlen;          // 硬件地址长度: 6
	uint8_t  hops;          // 跳数
	uint32_t xid;           // 事务ID
	uint16_t secs;          // 已过去秒数
	uint16_t flags;         // 标志位: 0x8000=广播
	uint32_t ciaddr;        // 客户端IP（续租时使用）
	uint32_t yiaddr;        // 分配的IP
	uint32_t siaddr;        // 服务器IP
	uint32_t giaddr;        // 网关IP（中继）
	uint8_t  chaddr[16];    // 客户端MAC地址
	uint8_t  sname[64];     // 服务器名称
	uint8_t  file[128];     // 启动文件名
	uint32_t cookie;        // DHCP Cookie: 0x63825363
	uint8_t  options[0];    // 选项数据
} DHCPPacket;
#pragma pack(pop)

// DHCP 配置信息
typedef struct {
	uint32_t assigned_ip;       // 分配的IP地址
	uint32_t subnet_mask;       // 子网掩码
	uint32_t gateway;           // 默认网关
	uint32_t dns_servers[4];    // DNS服务器列表
	int      dns_count;         // DNS服务器数量
	uint32_t lease_time;        // 租约时间（秒）
	uint32_t renewal_time;      // 续约时间T1（秒）
	uint32_t rebinding_time;     // 重绑定时间T2（秒）
	uint32_t server_ip;         // DHCP服务器IP
	char     domain_name[256];  // 域名
	uint8_t  vendor_specific[512]; // 厂商特定信息
	int      vendor_specific_len;
} DHCPConfig;

// 租约状态
typedef enum {
	LEASE_BOUND,        // 正常租用中
	LEASE_RENEWING,     // 尝试续约
	LEASE_REBINDING,    // 尝试重绑定
	LEASE_EXPIRED       // 租约过期
} LeaseState;

// 租约管理器
typedef struct {
	LeaseState  state;              // 当前状态
	time_t      acquired_time;      // 获取租约的时间
	time_t      renew_time;         // T1时间点
	time_t      rebind_time;        // T2时间点
	time_t      expire_time;        // 租约过期时间
	DHCPConfig  config;             // DHCP配置
	SOCKET      dhcp_socket;        // DHCP Socket
	uint32_t    xid;                // 当前事务ID
	int         adapter_index;      // 网卡索引
	char        adapter_desc[256];  // 网卡描述
} LeaseManager;

// 客户端配置
typedef struct {
	uint8_t mac[6];                 // MAC地址
	uint8_t client_id[7];           // 客户端ID
	int     client_id_len;
	uint8_t vendor_id[256];         // Vendor Class ID
	int     vendor_id_len;
	uint8_t param_req_list[16];     // 参数请求列表
	int     param_req_len;
} ClientConfig;

// 路由表条目
typedef struct {
	const char* network;
	int         mask_bits;          // 掩码位数（如24）
} RouteEntry;

// ============================================================================
// 全局变量
// ============================================================================

static ClientConfig g_client_config = {0};
static const RouteEntry g_routes_to_fix[] = {
	{"180.96.165.0", 24},
	{"180.96.143.0", 24},
	{"180.100.72.0", 24},
	{"180.100.47.0", 24}
};
static const int g_route_count = sizeof(g_routes_to_fix) / sizeof(g_routes_to_fix[0]);

// ============================================================================
// 函数声明
// ============================================================================

// 初始化函数
static void init_client_config(void);
static void init_default_config(void);

// DHCP包构建函数
static int  build_dhcp_discover(DHCPPacket* pkt, uint32_t xid, const ClientConfig* cfg);
static int  build_dhcp_request(DHCPPacket* pkt, uint32_t xid, uint32_t server_ip,
uint32_t requested_ip, const ClientConfig* cfg);
static int  build_dhcp_renew(DHCPPacket* pkt, uint32_t xid, uint32_t client_ip,
const ClientConfig* cfg);
static int  build_dhcp_rebind(DHCPPacket* pkt, uint32_t xid, uint32_t requested_ip,
const ClientConfig* cfg);

// DHCP响应解析
static int  parse_dhcp_response(const uint8_t* data, int len, DHCPConfig* config);
static int  get_message_type(const uint8_t* options, int len);

// 网卡操作函数
static void list_adapters(void);
static int  get_adapter_by_index(int idx, IP_ADAPTER_INFO* out);
static int  find_adapter_by_description(const char* target, IP_ADAPTER_INFO* out);
static int  get_interface_index(const char* adapter_desc);
static int  backup_nic_config(const char* adapter_desc);
static int  restore_nic_to_dhcp(const char* adapter_desc);
static int  configure_nic_ip(const char* adapter_desc, int if_index,
const DHCPConfig* config);
static int  configure_dns(const char* adapter_desc, int if_index,
const DHCPConfig* config);

// 路由操作函数
static int  delete_default_route(uint32_t gateway);
static int  fix_routes_to_gateway(uint32_t gateway, int if_index);

// DHCP流程函数
static int  dhcp_discover_offer(SOCKET sock, uint32_t xid, const ClientConfig* cfg,
uint32_t* out_server_ip, uint32_t* out_offered_ip);
static int  dhcp_request_ack(SOCKET sock, uint32_t xid, uint32_t server_ip,
uint32_t requested_ip, const ClientConfig* cfg,
DHCPConfig* out_config);
static int  full_dhcp_acquire(SOCKET sock, const ClientConfig* cfg, DHCPConfig* out_config);

// 租约管理函数
static int  attempt_renew(LeaseManager* lm, const ClientConfig* cfg);
static int  attempt_rebind(LeaseManager* lm, const ClientConfig* cfg);
static void enter_lease_management(LeaseManager* lm, const ClientConfig* cfg);

// 辅助函数
static bool is_admin(void);
static void restart_as_admin(void);
static int  parse_mac(const char* str, uint8_t mac[6]);
static void print_mac(const uint8_t mac[6]);
static void print_ip(uint32_t ip);
static void show_client_config(const ClientConfig* cfg);
static void print_usage(const char* prog);

// ============================================================================
// 初始化函数实现
// ============================================================================

static void init_default_config(void) {
	// 默认MAC地址
	const uint8_t default_mac[6] = {0x18, 0xeb, 0xd4, 0xbc, 0x0a, 0xf8};
	memcpy(g_client_config.mac, default_mac, 6);

	// Client ID: 0x01 + MAC
	g_client_config.client_id[0] = 0x01;
	memcpy(g_client_config.client_id + 1, default_mac, 6);
	g_client_config.client_id_len = 7;

	// 参数请求列表 (Option 55)
	const uint8_t param_list[] = {
		1,   // Subnet Mask
		3,   // Router
		6,   // DNS
		15,  // Domain Name
		26,  // Interface MTU
		28,  // Broadcast Address
		51,  // Lease Time
		58,  // Renewal T1
		59,  // Rebinding T2
		43,  // Vendor Specific
		114  // URL
	};
	memcpy(g_client_config.param_req_list, param_list, sizeof(param_list));
	g_client_config.param_req_len = sizeof(param_list);

	// Vendor ID (61字节)
	const uint8_t vendor_id[] = {
0x00, 0x00, 0x1f, 0x39, 0x01, 0xb9, 0x91, 0x7d, 0x72, 0x20, 0x29, 0x67, 0xb5, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x44, 0x50, 0x4c, 0xa0, 0x38, 0x43, 0x66, 0xd7, 0xeb, 0xe9, 0x43, 0x17, 
0x01, 0x1d, 0xad, 0xa1, 0x24, 0x78, 0x3d, 0x90, 0xca, 0x49, 0x83, 0xad, 0x1d, 0x71, 0x9f, 0xfe, 
0xb5, 0x38, 0x8e, 0x1d, 0x7d, 0xf6, 0x6b, 0x73, 0xbd, 0xcb, 0x31, 0x06, 0x84, 0x0c, 0x20, 0x30, 
0x30, 0x31, 0x30, 0x32, 0x31, 0x39, 0x39, 0x30, 0x31, 0x30, 0x34, 0x36, 0x32, 0x32, 0x30, 0x32, 
0x35, 0x34, 0x35, 0x31, 0x38, 0x45, 0x42, 0x44, 0x34, 0x42, 0x43, 0x30, 0x41, 0x46, 0x38, 0x37
	};
	memcpy(g_client_config.vendor_id, vendor_id, sizeof(vendor_id));
	g_client_config.vendor_id_len = sizeof(vendor_id);
}

static void init_client_config(void) {
	// 复制默认配置
	memcpy(&g_client_config, &g_client_config, sizeof(ClientConfig));
	// 注意：这里不应该使用默认MAC，而是使用实际网卡MAC
	// 实际MAC会在main中设置
}

// ============================================================================
// DHCP包构建函数实现
// ============================================================================

static int build_dhcp_discover(DHCPPacket* pkt, uint32_t xid, const ClientConfig* cfg) {
	memset(pkt, 0, sizeof(DHCPPacket));

	pkt->op = BOOTREQUEST;
	pkt->htype = 1;
	pkt->hlen = 6;
	pkt->xid = htonl(xid);
	pkt->flags = htons(0x8000);  // 广播标志

	memcpy(pkt->chaddr, cfg->mac, 6);
	pkt->cookie = htonl(DHCP_COOKIE);

	uint8_t* opt = pkt->options;

	// 消息类型: DHCP Discover
	*opt++ = OPTION_MSG_TYPE;
	*opt++ = 1;
	*opt++ = DHCPDISCOVER;

	// 最大消息大小
	*opt++ = OPTION_MAXSIZE;
	*opt++ = 2;
	*opt++ = 0x05;
	*opt++ = 0xdc;

	// 参数请求列表
	if (cfg->param_req_len > 0) {
		*opt++ = OPTION_PARAM_REQ_LIST;
		*opt++ = cfg->param_req_len;
		memcpy(opt, cfg->param_req_list, cfg->param_req_len);
		opt += cfg->param_req_len;
	}

	// Vendor ID
	if (cfg->vendor_id_len > 0) {
		*opt++ = OPTION_VENDOR_ID;
		*opt++ = cfg->vendor_id_len;
		memcpy(opt, cfg->vendor_id, cfg->vendor_id_len);
		opt += cfg->vendor_id_len;
	}

	// Client ID
	if (cfg->client_id_len > 0) {
		*opt++ = OPTION_CLIENT_ID;
		*opt++ = cfg->client_id_len;
		memcpy(opt, cfg->client_id, cfg->client_id_len);
		opt += cfg->client_id_len;
	}

	*opt++ = OPTION_END;

	return (int)(opt - (uint8_t*)pkt);
}

static int build_dhcp_request(DHCPPacket* pkt, uint32_t xid, uint32_t server_ip,
uint32_t requested_ip, const ClientConfig* cfg) {
	memset(pkt, 0, sizeof(DHCPPacket));

	pkt->op = BOOTREQUEST;
	pkt->htype = 1;
	pkt->hlen = 6;
	pkt->xid = htonl(xid);
	pkt->flags = htons(0x8000);

	memcpy(pkt->chaddr, cfg->mac, 6);
	pkt->cookie = htonl(DHCP_COOKIE);

	uint8_t* opt = pkt->options;

	// 消息类型: DHCP Request
	*opt++ = OPTION_MSG_TYPE;
	*opt++ = 1;
	*opt++ = DHCPREQUEST;

	// 请求的IP地址
	*opt++ = OPTION_REQUESTED_IP;
	*opt++ = 4;
	*(uint32_t*)opt = requested_ip;
	opt += 4;

	// 服务器标识
	*opt++ = OPTION_SERVER_ID;
	*opt++ = 4;
	*(uint32_t*)opt = server_ip;
	opt += 4;

	// Client ID
	if (cfg->client_id_len > 0) {
		*opt++ = OPTION_CLIENT_ID;
		*opt++ = cfg->client_id_len;
		memcpy(opt, cfg->client_id, cfg->client_id_len);
		opt += cfg->client_id_len;
	}

	// 参数请求列表
	if (cfg->param_req_len > 0) {
		*opt++ = OPTION_PARAM_REQ_LIST;
		*opt++ = cfg->param_req_len;
		memcpy(opt, cfg->param_req_list, cfg->param_req_len);
		opt += cfg->param_req_len;
	}

	*opt++ = OPTION_END;

	return (int)(opt - (uint8_t*)pkt);
}

static int build_dhcp_renew(DHCPPacket* pkt, uint32_t xid, uint32_t client_ip, const ClientConfig* cfg) 
{
	memset(pkt, 0, sizeof(DHCPPacket));

	pkt->op = BOOTREQUEST;
	pkt->htype = 1;
	pkt->hlen = 6;
	pkt->xid = htonl(xid);
	pkt->ciaddr = client_ip;        // 续租时填充当前IP
	pkt->flags = 0;                  // 单播

	memcpy(pkt->chaddr, cfg->mac, 6);
	pkt->cookie = htonl(DHCP_COOKIE);

	uint8_t* opt = pkt->options;

	// 消息类型: DHCP Request
	*opt++ = OPTION_MSG_TYPE;
	*opt++ = 1;
	*opt++ = DHCPREQUEST;

	// Client ID
	if (cfg->client_id_len > 0) {
		*opt++ = OPTION_CLIENT_ID;
		*opt++ = cfg->client_id_len;
		memcpy(opt, cfg->client_id, cfg->client_id_len);
		opt += cfg->client_id_len;
	}

	// 参数请求列表
	if (cfg->param_req_len > 0) {
		*opt++ = OPTION_PARAM_REQ_LIST;
		*opt++ = cfg->param_req_len;
		memcpy(opt, cfg->param_req_list, cfg->param_req_len);
		opt += cfg->param_req_len;
	}

	*opt++ = OPTION_END;

	return (int)(opt - (uint8_t*)pkt);
}

static int build_dhcp_rebind(DHCPPacket* pkt, uint32_t xid, uint32_t requested_ip,const ClientConfig* cfg) 
{
	memset(pkt, 0, sizeof(DHCPPacket));

	pkt->op = BOOTREQUEST;
	pkt->htype = 1;
	pkt->hlen = 6;
	pkt->xid = htonl(xid);
	pkt->flags = htons(0x8000);      // 广播

	memcpy(pkt->chaddr, cfg->mac, 6);
	pkt->cookie = htonl(DHCP_COOKIE);

	uint8_t* opt = pkt->options;

	// 消息类型: DHCP Request
	*opt++ = OPTION_MSG_TYPE;
	*opt++ = 1;
	*opt++ = DHCPREQUEST;

	// 请求的IP地址
	*opt++ = OPTION_REQUESTED_IP;
	*opt++ = 4;
	*(uint32_t*)opt = requested_ip;
	opt += 4;

	// Client ID
	if (cfg->client_id_len > 0) {
		*opt++ = OPTION_CLIENT_ID;
		*opt++ = cfg->client_id_len;
		memcpy(opt, cfg->client_id, cfg->client_id_len);
		opt += cfg->client_id_len;
	}

	// 参数请求列表
	if (cfg->param_req_len > 0) {
		*opt++ = OPTION_PARAM_REQ_LIST;
		*opt++ = cfg->param_req_len;
		memcpy(opt, cfg->param_req_list, cfg->param_req_len);
		opt += cfg->param_req_len;
	}

	*opt++ = OPTION_END;

	return (int)(opt - (uint8_t*)pkt);
}

// ============================================================================
// DHCP响应解析函数实现
// ============================================================================

static int get_message_type(const uint8_t* options, int len) 
{
	const uint8_t* opt = options;

	while (*opt != OPTION_END && (opt - options) < len - 1) 
	{
		uint8_t type = *opt++;
		uint8_t opt_len = *opt++;

		if (type == OPTION_MSG_TYPE && opt_len >= 1) 
		{
			return *opt;
		}
		opt += opt_len;
	}
	return 0;
}

static int parse_dhcp_response(const uint8_t* data, int len, DHCPConfig* config) 
{
	if (len < (int)sizeof(DHCPPacket) - 4) 
	{
		return -1;
	}

	DHCPPacket* pkt = (DHCPPacket*)data;
	if (ntohl(pkt->cookie) != DHCP_COOKIE) 
	{
		return -1;
	}

	memset(config, 0, sizeof(DHCPConfig));
	config->assigned_ip = pkt->yiaddr;
	config->server_ip = pkt->siaddr;

	printf("\n========== DHCP响应解析 ==========\n");
	printf("分配IP: ");
	print_ip(config->assigned_ip);

	uint8_t* opt = pkt->options;
	int msg_type = 0;

	while (*opt != OPTION_END && (opt - data) < len - 1) {
		uint8_t type = *opt++;
		uint8_t opt_len = *opt++;

		switch(type) {
			case OPTION_MSG_TYPE:
			if (opt_len >= 1) {
				msg_type = *opt;
				const char* type_str = (msg_type == DHCPOFFER) ? "DHCP Offer" :
				(msg_type == DHCPACK) ? "DHCP Ack" :
				(msg_type == DHCPNAK) ? "DHCP Nak" : "Unknown";
				printf("消息类型: %d (%s)\n", msg_type, type_str);
			}
			break;

			case OPTION_SUBNET_MASK:
			if (opt_len >= 4) {
				memcpy(&config->subnet_mask, opt, 4);
				printf("子网掩码: ");
				print_ip(config->subnet_mask);
			}
			break;

			case OPTION_ROUTER:
			if (opt_len >= 4) {
				memcpy(&config->gateway, opt, 4);
				printf("默认网关: ");
				print_ip(config->gateway);
			}
			break;

			case OPTION_DNS:
			config->dns_count = opt_len / 4;
			for (int i = 0; i < config->dns_count && i < 4; i++) {
				memcpy(&config->dns_servers[i], opt + i * 4, 4);
				printf("DNS服务器%d: ", i + 1);
				print_ip(config->dns_servers[i]);
			}
			break;

			case OPTION_DOMAIN_NAME:
			if (opt_len > 0 && opt_len < sizeof(config->domain_name)) {
				memcpy(config->domain_name, opt, opt_len);
				config->domain_name[opt_len] = '\0';
				printf("域名: %s\n", config->domain_name);
			}
			break;

			case OPTION_LEASE_TIME:
			if (opt_len >= 4) {
				memcpy(&config->lease_time, opt, 4);
				config->lease_time = ntohl(config->lease_time);
				printf("租约时间: %u 秒 (%u 分钟)\n", config->lease_time, config->lease_time / 60);
			}
			break;

			case OPTION_RENEWAL_T1:
			if (opt_len >= 4) {
				memcpy(&config->renewal_time, opt, 4);
				config->renewal_time = ntohl(config->renewal_time);
				printf("续约时间T1: %u 秒\n", config->renewal_time);
			}
			break;

			case OPTION_REBINDING_T2:
			if (opt_len >= 4) {
				memcpy(&config->rebinding_time, opt, 4);
				config->rebinding_time = ntohl(config->rebinding_time);
				printf("重绑定时间T2: %u 秒\n", config->rebinding_time);
			}
			break;

			case OPTION_VENDOR_SPECIFIC:
			if (opt_len > 0 && opt_len < sizeof(config->vendor_specific)) {
				memcpy(config->vendor_specific, opt, opt_len);
				config->vendor_specific_len = opt_len;
				printf("厂商特定信息: %d 字节\n", opt_len);
			}
			break;

			case OPTION_SERVER_ID:
			if (opt_len >= 4) {
				memcpy(&config->server_ip, opt, 4);
				printf("DHCP服务器: ");
				print_ip(config->server_ip);
			}
			break;
		}
		opt += opt_len;
	}

	printf("===================================\n");
    // 如果服务器没有提供 T1/T2，按 RFC 2131 计算默认值
    if (config->renewal_time == 0 && config->lease_time > 0) {
        config->renewal_time = config->lease_time / 2;      // 50%
        printf("续约时间T1未提供，使用默认值: %u 秒\n", config->renewal_time);
    }
    
    if (config->rebinding_time == 0 && config->lease_time > 0) {
        config->rebinding_time = (config->lease_time * 7) / 8;  // 87.5%
        printf("重绑定时间T2未提供，使用默认值: %u 秒\n", config->rebinding_time);
    }
    
    return (msg_type == DHCPACK) ? 1 : (msg_type == DHCPOFFER) ? 0 : -1;	
}

// ============================================================================
// 网卡操作函数实现
// ============================================================================

static void list_adapters(void) {
	PIP_ADAPTER_INFO pAdapterInfo, pAdapter;
	ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
	pAdapterInfo = (PIP_ADAPTER_INFO)malloc(sizeof(IP_ADAPTER_INFO));

	DWORD dwRet = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen);
	if (dwRet == ERROR_BUFFER_OVERFLOW) {
		free(pAdapterInfo);
		pAdapterInfo = (PIP_ADAPTER_INFO)malloc(ulOutBufLen);
		dwRet = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen);
	}

	if (dwRet != NO_ERROR) {
		printf("获取网卡信息失败\n");
		free(pAdapterInfo);
		return;
	}

	printf("\n========== 可用网卡列表 ==========\n");
	int index = 1;
	pAdapter = pAdapterInfo;
	while (pAdapter) {
		printf("%d. %s\n", index, pAdapter->Description);
		printf("   MAC: ");
		for (int i = 0; i < 6; i++) {
			printf("%02X%s", pAdapter->Address[i], i < 5 ? ":" : "");
		}
		printf("\n   IP: %s\n", pAdapter->IpAddressList.IpAddress.String);
		pAdapter = pAdapter->Next;
		index++;
	}
	printf("==================================\n\n");

	free(pAdapterInfo);
}

static int get_adapter_by_index(int idx, IP_ADAPTER_INFO* out) {
	PIP_ADAPTER_INFO pAdapterInfo, pAdapter;
	ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
	pAdapterInfo = (PIP_ADAPTER_INFO)malloc(sizeof(IP_ADAPTER_INFO));

	DWORD dwRet = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen);
	if (dwRet == ERROR_BUFFER_OVERFLOW) {
		free(pAdapterInfo);
		pAdapterInfo = (PIP_ADAPTER_INFO)malloc(ulOutBufLen);
		dwRet = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen);
	}

	if (dwRet != NO_ERROR) {
		free(pAdapterInfo);
		return -1;
	}

	int current = 1;
	pAdapter = pAdapterInfo;
	while (pAdapter && current < idx) {
		pAdapter = pAdapter->Next;
		current++;
	}

	if (pAdapter && current == idx) {
		memcpy(out, pAdapter, sizeof(IP_ADAPTER_INFO));
		free(pAdapterInfo);
		return 0;
	}

	free(pAdapterInfo);
	return -1;
}

static int find_adapter_by_description(const char* target, IP_ADAPTER_INFO* out) {
	PIP_ADAPTER_INFO pAdapterInfo, pAdapter;
	ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
	pAdapterInfo = (PIP_ADAPTER_INFO)malloc(sizeof(IP_ADAPTER_INFO));

	DWORD dwRet = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen);
	if (dwRet == ERROR_BUFFER_OVERFLOW) {
		free(pAdapterInfo);
		pAdapterInfo = (PIP_ADAPTER_INFO)malloc(ulOutBufLen);
		dwRet = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen);
	}

	if (dwRet != NO_ERROR) {
		free(pAdapterInfo);
		return -1;
	}

	int index = 1;
	pAdapter = pAdapterInfo;
	while (pAdapter) {
		if (strstr(pAdapter->Description, target) != NULL) {
			memcpy(out, pAdapter, sizeof(IP_ADAPTER_INFO));
			free(pAdapterInfo);
			return index;
		}
		pAdapter = pAdapter->Next;
		index++;
	}

	free(pAdapterInfo);
	return -1;
}

static int get_interface_index(const char* adapter_desc) {
	ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
	ULONG family = AF_UNSPEC;
	ULONG outBufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(outBufLen);

	DWORD dwRet = GetAdaptersAddresses(family, flags, NULL, pAddresses, &outBufLen);
	if (dwRet == ERROR_BUFFER_OVERFLOW) {
		free(pAddresses);
		pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(outBufLen);
		dwRet = GetAdaptersAddresses(family, flags, NULL, pAddresses, &outBufLen);
	}

	if (dwRet != NO_ERROR) {
		free(pAddresses);
		return -1;
	}

	wchar_t wdesc[256];
	mbstowcs(wdesc, adapter_desc, 256);

	PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
	while (pCurr) {
		if (pCurr->Description && wcsstr(pCurr->Description, wdesc) != NULL) {
			int if_index = pCurr->IfIndex;
			free(pAddresses);
			return if_index;
		}
		pCurr = pCurr->Next;
	}

	free(pAddresses);
	return -1;
}

static int backup_nic_config(const char* adapter_desc) {
	char cmd[512];
	// 将描述中的空格替换为下划线作为文件名
	char filename[256];
	strncpy(filename, adapter_desc, sizeof(filename) - 1);
	filename[sizeof(filename) - 1] = '\0';
	for (char* p = filename; *p; p++) {
		if (*p == ' ') *p = '_';
	}

	snprintf(cmd, sizeof(cmd), "netsh interface ip dump > \"%s_nic_backup.txt\"", filename);
	system(cmd);
	printf("已备份当前配置到 %s_nic_backup.txt\n", filename);
	return 0;
}

static int restore_nic_to_dhcp(const char* adapter_desc) {
	char cmd[512];
	printf("\n恢复网卡为DHCP自动获取...\n");

	snprintf(cmd, sizeof(cmd), "netsh interface ip set address \"%s\" dhcp", adapter_desc);
	printf("执行: %s\n", cmd);
	system(cmd);

	snprintf(cmd, sizeof(cmd), "netsh interface ip set dns \"%s\" dhcp", adapter_desc);
	printf("执行: %s\n", cmd);
	system(cmd);

	printf("已恢复为DHCP模式\n");
	return 0;
}

static int configure_nic_ip(const char* adapter_desc, int if_index, const DHCPConfig* config) {
	if (config->assigned_ip == 0 || config->subnet_mask == 0) {
		printf("错误: 未获取到有效的IP地址或子网掩码\n");
		return -1;
	}

	char ip_str[32], mask_str[32], gateway_str[32];
	struct in_addr addr;

	addr.s_addr = config->assigned_ip;
	strcpy(ip_str, inet_ntoa(addr));

	addr.s_addr = config->subnet_mask;
	strcpy(mask_str, inet_ntoa(addr));

	printf("\n========== 配置网卡 ==========\n");
	printf("网卡: %s (索引: %d)\n", adapter_desc, if_index);
	printf("IP: %s\n", ip_str);
	printf("子网掩码: %s\n", mask_str);

	char cmd[512];
	if (config->gateway != 0) {
		addr.s_addr = config->gateway;
		strcpy(gateway_str, inet_ntoa(addr));
		printf("网关: %s\n", gateway_str);
		snprintf(cmd, sizeof(cmd),
		"netsh interface ip set address name=\"%d\" static %s %s gateway=%s gwmetric=1",
		if_index, ip_str, mask_str, gateway_str);
	}
	else {
		snprintf(cmd, sizeof(cmd),
		"netsh interface ip set address name=\"%d\" static %s %s",
		if_index, ip_str, mask_str);
	}

	printf("执行: %s\n", cmd);
	int result = system(cmd);

	if (result != 0) {
		printf("设置IP地址失败\n");
		return -1;
	}

	printf("IP地址配置成功\n");
	printf("================================\n");
	return 0;
}

static int configure_dns(const char* adapter_desc, int if_index, const DHCPConfig* config) {
	if (config->dns_count == 0) {
		return 0;
	}

	char cmd[512];
	printf("\n========== 配置DNS ==========\n");

	// 清除现有DNS设置
	snprintf(cmd, sizeof(cmd), "netsh interface ip delete dnsservers name=\"%d\" all", if_index);
	system(cmd);

	// 使用127.0.0.1作为DNS，避免大量DNS查询走IPTV网口
	// （根据实际需求，这里可以改为config->dns_servers[0]）
	snprintf(cmd, sizeof(cmd),
	"netsh interface ip set dnsservers name=\"%d\" static 127.0.0.1 primary",
	if_index);
	printf("执行: %s\n", cmd);
	system(cmd);

	printf("DNS配置成功\n");
	printf("================================\n");
	return 0;
}

// ============================================================================
// 路由操作函数实现
// ============================================================================

static int delete_default_route(uint32_t gateway) {
	struct in_addr addr;
	addr.s_addr = gateway;
	char gateway_str[32];
	strcpy(gateway_str, inet_ntoa(addr));

	char cmd[256];
	printf("\n========== 删除默认路由 ==========\n");

	snprintf(cmd, sizeof(cmd), "route delete 0.0.0.0 mask 0.0.0.0 %s", gateway_str);
	printf("执行: %s\n", cmd);
	int result = system(cmd);

	if (result != 0) {
		// 尝试备用方式
		snprintf(cmd, sizeof(cmd), "route delete 0.0.0.0");
		printf("备用命令: %s\n", cmd);
		result = system(cmd);
	}

	if (result == 0) {
		printf("默认路由删除成功\n");
	}
	else {
		printf("默认路由删除失败或不存在\n");
	}

	printf("===================================\n");
	return result;
}

static int fix_routes_to_gateway(uint32_t gateway, int if_index) {
	struct in_addr addr;
	addr.s_addr = gateway;
	char gateway_str[32];
	strcpy(gateway_str, inet_ntoa(addr));

	printf("\n========== 修复路由表 ==========\n");

	for (int i = 0; i < g_route_count; i++) {
		char cmd[256];

		// 删除原有路由
		snprintf(cmd, sizeof(cmd), "route delete %s", g_routes_to_fix[i].network);
		printf("执行: %s\n", cmd);
		system(cmd);

		// 添加到指定网关
		snprintf(cmd, sizeof(cmd),
		"netsh interface ip add route %s/%d %d %s",
		g_routes_to_fix[i].network,
		g_routes_to_fix[i].mask_bits,
		if_index,
		gateway_str);

		printf("执行: %s\n", cmd);
		int result = system(cmd);

		if (result == 0) {
			printf("  ✓ %s -> %s\n", g_routes_to_fix[i].network, gateway_str);
		}
		else {
			printf("  ✗ %s 添加失败\n", g_routes_to_fix[i].network);
		}
	}

	printf("===================================\n");
	return 0;
}

// ============================================================================
// DHCP流程函数实现
// ============================================================================

static bool is_lan_response(uint32_t ip) 
{
	
    uint8_t* bytes = (uint8_t*)&ip;
    printf("判断是不是本网内DHCP %d.%d.%d.%d\n", bytes[0], bytes[1], bytes[2], bytes[3]);
    // 忽略 192.168.x.x
    if (bytes[0] == 192 && bytes[1] == 168) {
        printf("  -> 忽略：192.168.x.x 网段\n");
        return true;
    }
    // 忽略 172.16-31.x.x
    if (bytes[0] == 172 && (bytes[1] >= 16 && bytes[1] <= 31)) {
        printf("  -> 忽略：172.16-31.x.x 网段\n");
        return true;
    }
    return false;
}
static int dhcp_discover_offer(SOCKET sock, uint32_t xid, const ClientConfig* cfg, uint32_t* out_server_ip, uint32_t* out_offered_ip) 
{
	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = INADDR_BROADCAST;
	dest.sin_port = htons(DHCP_SERVER_PORT);

	// 发送DHCP Discover
	uint8_t buffer[1024];
	DHCPPacket* pkt = (DHCPPacket*)buffer;
	int len = build_dhcp_discover(pkt, xid, cfg);

	int result = sendto(sock, (char*)buffer, len, 0, (struct sockaddr*)&dest, sizeof(dest));
	if (result == SOCKET_ERROR) {
		printf("发送Discover失败: %d\n", WSAGetLastError());
		return -1;
	}

	printf("已发送DHCP Discover (%d字节)\n", result);
	printf("等待DHCP Offer...\n");

	time_t start_time = time(NULL);
	while (time(NULL) - start_time < DISCOVER_TIMEOUT) {
		fd_set fds;
		struct timeval tv;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(0, &fds, NULL, NULL, &tv) > 0) {
			uint8_t recv_buf[2048];
			struct sockaddr_in from;
			int from_len = sizeof(from);
			int recv_len = recvfrom(sock, (char*)recv_buf, sizeof(recv_buf), 0,
			(struct sockaddr*)&from, &from_len);
			if (recv_len > 0) {
				// 忽略局域网响应
				if (is_lan_response(from.sin_addr.s_addr)) {
					continue;
				}

				DHCPPacket* resp = (DHCPPacket*)recv_buf;
				if (ntohl(resp->xid) != xid) {
					continue;
				}

				int msg_type = get_message_type(resp->options, recv_len - sizeof(DHCPPacket));
				if (msg_type == DHCPOFFER) {
					DHCPConfig temp;
					parse_dhcp_response(recv_buf, recv_len, &temp);

					*out_offered_ip = temp.assigned_ip;
					*out_server_ip = temp.server_ip;
					if (*out_server_ip == 0) {
						*out_server_ip = from.sin_addr.s_addr;
					}

					printf("收到DHCP Offer\n");
					return 0;
				}
			}
		}
	}

	printf("未收到DHCP Offer\n");
	return -1;
}

static int dhcp_request_ack(SOCKET sock, uint32_t xid, uint32_t server_ip, uint32_t requested_ip, const ClientConfig* cfg, DHCPConfig* out_config) 
{
	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = INADDR_BROADCAST;
	dest.sin_port = htons(DHCP_SERVER_PORT);

	// 发送DHCP Request
	uint8_t buffer[1024];
	DHCPPacket* pkt = (DHCPPacket*)buffer;
	int len = build_dhcp_request(pkt, xid, server_ip, requested_ip, cfg);

	sendto(sock, (char*)buffer, len, 0, (struct sockaddr*)&dest, sizeof(dest));

	printf("已发送DHCP Request (请求IP: ");
	print_ip(requested_ip);
	printf(")\n等待DHCP ACK...\n");

	time_t start_time = time(NULL);
	while (time(NULL) - start_time < DISCOVER_TIMEOUT) {
		fd_set fds;
		struct timeval tv;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(0, &fds, NULL, NULL, &tv) > 0) {
			uint8_t recv_buf[2048];
			struct sockaddr_in from;
			int from_len = sizeof(from);
			int recv_len = recvfrom(sock, (char*)recv_buf, sizeof(recv_buf), 0,
			(struct sockaddr*)&from, &from_len);
			if (recv_len > 0) {
				// 忽略局域网响应
				if (is_lan_response(from.sin_addr.s_addr)) {
					continue;
				}

				DHCPPacket* resp = (DHCPPacket*)recv_buf;
				if (ntohl(resp->xid) != xid) {
					continue;
				}

				int result = parse_dhcp_response(recv_buf, recv_len, out_config);
				if (result == 1) {  // DHCP ACK
					printf("收到DHCP ACK！\n");
					return 0;
				}
				else if (result == -1) {  // 可能是NAK
					int msg_type = get_message_type(resp->options, recv_len - sizeof(DHCPPacket));
					if (msg_type == DHCPNAK) {
						printf("收到DHCP NAK，请求被拒绝\n");
						return -2;
					}
				}
			}
		}
	}

	printf("未收到DHCP ACK\n");
	return -1;
}

static int full_dhcp_acquire(SOCKET sock, const ClientConfig* cfg, DHCPConfig* out_config) 
{
	printf("\n========== 开始完整DHCP获取流程 ==========\n");

	uint32_t xid = (uint32_t)rand();
	printf("事务ID: %08X\n", xid);

	for (int attempt = 0; attempt < MAX_DHCP_RETRIES; attempt++) {
		printf("\n---- 第 %d 次尝试 ----\n", attempt + 1);

		uint32_t server_ip = 0, offered_ip = 0;

		// DHCP Discover/Offer
		if (dhcp_discover_offer(sock, xid, cfg, &server_ip, &offered_ip) != 0) {
			if (attempt < MAX_DHCP_RETRIES - 1) Sleep(3000);
			continue;
		}

		// DHCP Request/ACK
		int result = dhcp_request_ack(sock, xid, server_ip, offered_ip, cfg, out_config);
		if (result == 0) {
			printf("========== DHCP获取成功 ==========\n");
			return 0;
		}

		if (attempt < MAX_DHCP_RETRIES - 1) Sleep(3000);
	}

	printf("========== DHCP获取失败 ==========\n");
	return -1;
}

// ============================================================================
// 租约管理函数实现
// ============================================================================

static int attempt_renew(LeaseManager* lm, const ClientConfig* cfg) 
{
	lm->xid = (uint32_t)rand();

	uint8_t buffer[1024];
	DHCPPacket* pkt = (DHCPPacket*)buffer;
	int len = build_dhcp_renew(pkt, lm->xid, lm->config.assigned_ip, cfg);

	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = lm->config.server_ip;
	dest.sin_port = htons(DHCP_SERVER_PORT);

	sendto(lm->dhcp_socket, (char*)buffer, len, 0, (struct sockaddr*)&dest, sizeof(dest));

	printf("已发送续租请求到DHCP服务器: ");
	print_ip(lm->config.server_ip);
	printf("\n");

	// 等待响应
	time_t start_time = time(NULL);
	while (time(NULL) - start_time < RENEW_TIMEOUT) {
		fd_set fds;
		struct timeval tv;
		FD_ZERO(&fds);
		FD_SET(lm->dhcp_socket, &fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(0, &fds, NULL, NULL, &tv) > 0) {
			uint8_t recv_buf[2048];
			struct sockaddr_in from;
			int from_len = sizeof(from);
			int recv_len = recvfrom(lm->dhcp_socket, (char*)recv_buf, sizeof(recv_buf), 0,
			(struct sockaddr*)&from, &from_len);
			if (recv_len > 0) {
				DHCPPacket* resp = (DHCPPacket*)recv_buf;
				if (ntohl(resp->xid) == lm->xid) {
					DHCPConfig new_config;
					int is_ack = parse_dhcp_response(recv_buf, recv_len, &new_config);
					if (is_ack == 1) {
						memcpy(&lm->config, &new_config, sizeof(DHCPConfig));
						lm->acquired_time = time(NULL);
						lm->renew_time = lm->acquired_time + lm->config.renewal_time;
						lm->rebind_time = lm->acquired_time + lm->config.rebinding_time;
						lm->expire_time = lm->acquired_time + lm->config.lease_time;
						lm->state = LEASE_BOUND;
						printf("✓ 续租成功！新租约至 %s", ctime(&lm->expire_time));
						return 0;
					}
				}
			}
		}
	}

	printf("续租失败\n");
	return -1;
}

static int attempt_rebind(LeaseManager* lm, const ClientConfig* cfg) 
{
	lm->xid = (uint32_t)rand();

	uint8_t buffer[1024];
	DHCPPacket* pkt = (DHCPPacket*)buffer;
	int len = build_dhcp_rebind(pkt, lm->xid, lm->config.assigned_ip, cfg);

	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = INADDR_BROADCAST;
	dest.sin_port = htons(DHCP_SERVER_PORT);

	sendto(lm->dhcp_socket, (char*)buffer, len, 0, (struct sockaddr*)&dest, sizeof(dest));
	printf("已发送重绑定请求（广播）\n");

	time_t last_send = time(NULL);

	while (time(NULL) < lm->expire_time) {
		fd_set fds;
		struct timeval tv;
		FD_ZERO(&fds);
		FD_SET(lm->dhcp_socket, &fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(0, &fds, NULL, NULL, &tv) > 0) {
			uint8_t recv_buf[2048];
			struct sockaddr_in from;
			int from_len = sizeof(from);
			int recv_len = recvfrom(lm->dhcp_socket, (char*)recv_buf, sizeof(recv_buf), 0,
			(struct sockaddr*)&from, &from_len);
			if (recv_len > 0) {
				DHCPPacket* resp = (DHCPPacket*)recv_buf;
				if (ntohl(resp->xid) == lm->xid) {
					DHCPConfig new_config;
					int is_ack = parse_dhcp_response(recv_buf, recv_len, &new_config);
					if (is_ack == 1) {
						memcpy(&lm->config, &new_config, sizeof(DHCPConfig));
						lm->acquired_time = time(NULL);
						lm->renew_time = lm->acquired_time + lm->config.renewal_time;
						lm->rebind_time = lm->acquired_time + lm->config.rebinding_time;
						lm->expire_time = lm->acquired_time + lm->config.lease_time;
						lm->state = LEASE_BOUND;
						printf("✓ 重绑定成功！新租约至 %s", ctime(&lm->expire_time));

						// 如果IP或网关变了，重新配置网卡
						if (new_config.assigned_ip != lm->config.assigned_ip ||
						new_config.gateway != lm->config.gateway) {
							printf("IP地址变更为 ");
							print_ip(new_config.assigned_ip);
							printf("，重新配置网卡...\n");
							configure_nic_ip(lm->adapter_desc, lm->adapter_index, &new_config);
						}
						return 0;
					}
				}
			}
		}

		// 每2秒重发一次请求
		if (time(NULL) - last_send >= REBIND_RETRY_INTERVAL) {
			len = build_dhcp_rebind(pkt, lm->xid, lm->config.assigned_ip, cfg);
			sendto(lm->dhcp_socket, (char*)buffer, len, 0, (struct sockaddr*)&dest, sizeof(dest));
			last_send = time(NULL);
			printf("重发重绑定请求...\n");
		}
	}

	printf("重绑定失败，租约已过期\n");
	return -1;
}

static void enter_lease_management(LeaseManager* lm, const ClientConfig* cfg) 
{
	    // 确保 T1/T2 有效（再次检查）
    if (lm->config.renewal_time == 0 && lm->config.lease_time > 0) {
        lm->config.renewal_time = lm->config.lease_time / 2;
    }
    if (lm->config.rebinding_time == 0 && lm->config.lease_time > 0) {
        lm->config.rebinding_time = (lm->config.lease_time * 7) / 8;
    }
    
    lm->acquired_time = time(NULL);
    lm->renew_time = lm->acquired_time + lm->config.renewal_time;
    lm->rebind_time = lm->acquired_time + lm->config.rebinding_time;
    lm->expire_time = lm->acquired_time + lm->config.lease_time;
    
	lm->acquired_time = time(NULL);
	lm->renew_time = lm->acquired_time + lm->config.renewal_time;
	lm->rebind_time = lm->acquired_time + lm->config.rebinding_time;
	lm->expire_time = lm->acquired_time + lm->config.lease_time;
	lm->state = LEASE_BOUND;

	printf("\n========== 进入租约管理 ==========\n");
	printf("租约获取时间: %s", ctime(&lm->acquired_time));
	printf("续约时间 T1: %s", ctime(&lm->renew_time));
	printf("重绑定时间 T2: %s", ctime(&lm->rebind_time));
	printf("租约过期时间: %s", ctime(&lm->expire_time));
	printf("===================================\n");

	while (1) {
		time_t now = time(NULL);

		switch (lm->state) {
			case LEASE_BOUND:
			if (now >= lm->renew_time) {
				printf("\n[%.0f秒] 进入RENEWING状态，开始续租...\n", difftime(now, lm->acquired_time));
				lm->state = LEASE_RENEWING;
				attempt_renew(lm, cfg);
			}
			break;

			case LEASE_RENEWING:
			if (now >= lm->rebind_time) {
				printf("\n[%.0f秒] 续租失败，进入REBINDING状态...\n", difftime(now, lm->acquired_time));
				lm->state = LEASE_REBINDING;
				attempt_rebind(lm, cfg);
			}
			else if (now < lm->renew_time) {
				// 续租成功，回到BOUND状态
				lm->state = LEASE_BOUND;
			}
			break;

			case LEASE_REBINDING:
			if (now >= lm->expire_time) {
				printf("租约已过期，尝试重新获取...\n");
				lm->state = LEASE_EXPIRED;
			}
			else if (now < lm->rebind_time) {
				// 重绑定成功，回到BOUND状态
				lm->state = LEASE_BOUND;
			}
			break;

			case LEASE_EXPIRED:
			{
				DHCPConfig new_config;
				if (full_dhcp_acquire(lm->dhcp_socket, cfg, &new_config) == 0) {
					memcpy(&lm->config, &new_config, sizeof(DHCPConfig));
					lm->acquired_time = time(NULL);
					lm->renew_time = lm->acquired_time + lm->config.renewal_time;
					lm->rebind_time = lm->acquired_time + lm->config.rebinding_time;
					lm->expire_time = lm->acquired_time + lm->config.lease_time;
					lm->state = LEASE_BOUND;

					// 重新配置网卡
					configure_nic_ip(lm->adapter_desc, lm->adapter_index, &lm->config);
					if (lm->config.gateway != 0) {
						delete_default_route(lm->config.gateway);
						fix_routes_to_gateway(lm->config.gateway, lm->adapter_index);
					}

					printf("重新获取成功！新租约至 %s", ctime(&lm->expire_time));
				}
				else {
					printf("重新获取失败，5秒后重试...\n");
					Sleep(5000);
				}
			}
			break;
		}

		Sleep(1000);
	}
}

// ============================================================================
// 辅助函数实现
// ============================================================================

static bool is_admin(void) 
{
	BOOL is_admin = FALSE;
	PSID admin_group_sid = NULL;
	SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;

	if (AllocateAndInitializeSid(&nt_authority, 2,
	SECURITY_BUILTIN_DOMAIN_RID,
	DOMAIN_ALIAS_RID_ADMINS,
	0, 0, 0, 0, 0, 0,
	&admin_group_sid)) {
		CheckTokenMembership(NULL, admin_group_sid, &is_admin);
		FreeSid(admin_group_sid);
	}

	return is_admin == TRUE;
}

static void restart_as_admin(void) {
	char path[MAX_PATH];
	GetModuleFileNameA(NULL, path, MAX_PATH);

	SHELLEXECUTEINFOA sei = {0};
	sei.cbSize = sizeof(sei);
	sei.lpVerb = "runas";
	sei.lpFile = path;
	sei.lpParameters = GetCommandLineA();
	sei.nShow = SW_SHOW;

	if (!ShellExecuteExA(&sei)) {
		printf("请求管理员权限失败: %lu\n", GetLastError());
	}
}

static int parse_mac(const char* str, uint8_t mac[6]) {
	int values[6];
	if (sscanf(str, "%x:%x:%x:%x:%x:%x",
	&values[0], &values[1], &values[2],
	&values[3], &values[4], &values[5]) != 6) {
		return -1;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = (uint8_t)values[i];
	}
	return 0;
}

static void print_mac(const uint8_t mac[6]) {
	for (int i = 0; i < 6; i++) {
		printf("%02X%s", mac[i], i < 5 ? ":" : "");
	}
}

static void print_ip(uint32_t ip) {
	struct in_addr addr;
	addr.s_addr = ip;
	printf("%s", inet_ntoa(addr));
}

static void show_client_config(const ClientConfig* cfg) 
{
	printf("\n========== 客户端配置 ==========\n");
	printf("MAC地址:        ");
	print_mac(cfg->mac);
	printf("\nClient ID:      ");
	for (int i = 0; i < cfg->client_id_len; i++) {
		printf("%02X", cfg->client_id[i]);
	}
	printf("\nVendor ID:      ");
	for (int i = 0; i < cfg->vendor_id_len && i < 100; i++) {
			printf("0x%02X ", cfg->vendor_id[i]);
			if((i+1) % 16 == 0)
				printf("\n");

	}
	printf("\n================================\n\n");
}

static void print_usage(const char* prog) 
{
	printf("用法: %s [选项]\n\n", prog);
	printf("选项:\n");
	printf("  -l, --list                 列出所有网卡\n");
	printf("  -n, --netcard <索引>       选择网卡 (1,2,3...)\n");
	printf("  -m, --mac <MAC>            设置MAC地址 (格式: xx:xx:xx:xx:xx:xx)\n");
	printf("  -r, --restore              恢复网卡为DHCP模式\n");
	printf("  --show-default             显示默认配置\n");
	printf("  -h, --help                 显示此帮助\n\n");
	printf("示例:\n");
	printf("  %s -l                                    # 列出网卡\n", prog);
	printf("  %s -n 1                                  # 使用第1块网卡\n", prog);
	printf("  %s -n 2 -m 18:eb:d4:bc:0a:f8            # 指定网卡和MAC\n", prog);
	printf("  %s -n 1 -r                               # 恢复网卡为DHCP\n", prog);
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) 
{
	// 设置控制台编码为UTF-8
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);

	// 检查管理员权限
	if (!is_admin()) {
		printf("需要管理员权限，正在请求...\n");
		restart_as_admin();
		return 0;
	}

	// 初始化Winsock
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		printf("WSAStartup失败\n");
		return 1;
	}

	srand((unsigned int)time(NULL));
	init_default_config();

	// 命令行参数解析
	int netcard_index = 0;
	int list_only = 0;
	int restore_mode = 0;
	int show_default = 0;
	uint8_t custom_mac[6];
	int mac_set = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
			list_only = 1;
		}
		else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--restore") == 0) {
			restore_mode = 1;
		}
		else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--netcard") == 0) {
			if (i + 1 < argc) netcard_index = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mac") == 0) {
			if (i + 1 < argc && parse_mac(argv[++i], custom_mac) == 0) {
				mac_set = 1;
			}
			else {
				printf("MAC格式错误\n");
				WSACleanup();
				return 1;
			}
		}
		else if (strcmp(argv[i], "--show-default") == 0) {
			show_default = 1;
		}
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			WSACleanup();
			return 0;
		}
	}

	// 显示默认配置
	if (show_default) {
		show_client_config(&g_client_config);
		WSACleanup();
		return 0;
	}

	// 列出网卡
	list_adapters();

	if (list_only) {
		WSACleanup();
		return 0;
	}

	// 获取选中的网卡
	IP_ADAPTER_INFO adapter_info;
	if (netcard_index == 0) {
		// 自动检测目标网卡（包含"USB"描述）
		int auto_index = find_adapter_by_description("USB", &adapter_info);
		if (auto_index > 0) {
			printf("\n========== 自动检测到目标网卡 ==========\n");
			printf("自动选择网卡 %d: %s\n", auto_index, adapter_info.Description);
			netcard_index = auto_index;
		}
		else {
			printf("\n请使用 -n 参数选择网卡 (如 -n 1)\n");
			WSACleanup();
			return 1;
		}
	}
	else {
		if (get_adapter_by_index(netcard_index, &adapter_info) != 0) {
			printf("错误: 无效的网卡索引 %d\n", netcard_index);
			WSACleanup();
			return 1;
		}
	}

	printf("已选择网卡: %s\n", adapter_info.Description);

	// 恢复模式
	if (restore_mode) {
		restore_nic_to_dhcp(adapter_info.Description);
		WSACleanup();
		return 0;
	}

	// 使用网卡的实际MAC（如果用户没有指定）
	if (!mac_set) {
		memcpy(g_client_config.mac, adapter_info.Address, 6);
		// 更新Client ID中的MAC部分
		memcpy(g_client_config.client_id + 1, adapter_info.Address, 6);
		printf("使用网卡实际MAC地址: ");
		print_mac(g_client_config.mac);
		printf("\n");
	}
	else {
		memcpy(g_client_config.mac, custom_mac, 6);
		memcpy(g_client_config.client_id + 1, custom_mac, 6);
		printf("使用自定义MAC地址: ");
		print_mac(g_client_config.mac);
		printf("\n");
	}

	// 获取接口索引
	int if_index = get_interface_index(adapter_info.Description);
	printf("网卡接口索引: %d\n", if_index);

	// 显示最终配置
	show_client_config(&g_client_config);

	// 创建DHCP Socket
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET) {
		printf("创建socket失败: %d\n", WSAGetLastError());
		WSACleanup();
		return 1;
	}

	// 绑定到68端口
	struct sockaddr_in bind_addr;
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = INADDR_ANY;
	bind_addr.sin_port = htons(DHCP_CLIENT_PORT);

	if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
		printf("绑定端口68失败，请以管理员身份运行\n");
		closesocket(sock);
		WSACleanup();
		return 1;
	}

	// 设置广播选项
	BOOL broadcast = TRUE;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

	// 设置接收超时
	int timeout = DISCOVER_TIMEOUT * 1000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

	// 执行DHCP获取
	DHCPConfig dhcp_config;
	if (full_dhcp_acquire(sock, &g_client_config, &dhcp_config) != 0) {
		printf("DHCP获取失败，程序退出\n");
		closesocket(sock);
		WSACleanup();
		getch();
		return 1;
	}

	// 备份当前配置
	backup_nic_config(adapter_info.Description);

	// 配置网卡
	if (configure_nic_ip(adapter_info.Description, if_index, &dhcp_config) != 0) {
		printf("配置网卡失败\n");
		closesocket(sock);
		WSACleanup();
		return 1;
	}

	// 配置DNS
	configure_dns(adapter_info.Description, if_index, &dhcp_config);

	// 删除默认路由并修复路由表
	if (dhcp_config.gateway != 0) {
		delete_default_route(dhcp_config.gateway);
		fix_routes_to_gateway(dhcp_config.gateway, if_index);
	}

	printf("\n========== 配置完成！ ==========\n");
	printf("进入租约管理...\n");
	//    getch();

	// 进入租约管理循环
	LeaseManager lm;
	memset(&lm, 0, sizeof(lm));
	lm.dhcp_socket = sock;
	lm.adapter_index = if_index;
	strncpy(lm.adapter_desc, adapter_info.Description, sizeof(lm.adapter_desc) - 1);
	lm.adapter_desc[sizeof(lm.adapter_desc) - 1] = '\0';
	memcpy(&lm.config, &dhcp_config, sizeof(DHCPConfig));

	enter_lease_management(&lm, &g_client_config);

	// 正常情况下不会执行到这里
	closesocket(sock);
	WSACleanup();
	return 0;
}