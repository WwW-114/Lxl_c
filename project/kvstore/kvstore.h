#ifndef KVSTORE_H_
#define KVSTORE_H_

#include <stddef.h>
#include "kvstore_config.h"

/*
 * 处理一条完整的 KV 请求行。
 *
 * request       : 请求内容，不要求以 '\0' 结尾
 * request_len   : 请求长度，不含行尾 '\n'
 * response      : 调用方提供的响应缓冲区
 * response_cap  : 响应缓冲区容量
 * response_len  : 输出响应长度，不含结尾换行
 *
 * 返回 0  : 已生成响应（包括协议错误响应）
 * 返回 -1 : 内部错误（参数非法、响应缓冲区不足等）
 *
 * 响应内容不带结尾 '\n'，由网络层统一追加。
 */
int kvstore_handle_request (const char *request, size_t request_len,
                            char *response, size_t response_cap, size_t *response_len);

#endif
