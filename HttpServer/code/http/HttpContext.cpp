#include "HttpContext.h"

using namespace muduo;
using namespace muduo::net;

namespace http
{

	// 将报文解析出来，将关键信息封装到 HttpRequest 对象里面
	bool HttpContext::parseRequest(Buffer* buf, Timestamp receiveTime)
	{
		bool ok = true; // 解析每行请求格式是否正确的标志
		bool hasMore = true; // 标志是否还有更多数据需要解析

		while (hasMore)
		{
			if (state_ == kExpectRequestLine) // 当前状态：期望解析请求行
			{
				const char* crlf = buf->findCRLF(); // 查找 CRLF (\r\n) 的位置，表示请求行结束
				if (crlf) // 找到了完整的请求行
				{
					ok = processRequestLine(buf->peek(), crlf); // 解析请求行内容（方法、路径、HTTP版本）
					if (ok)
					{
						request_.setReceiveTime(receiveTime); // 设置请求接收时间
						buf->retrieveUntil(crlf + 2); // 移动读指针到请求行后面（跳过 \r\n）
						state_ = kExpectHeaders; // 切换状态，下一步解析请求头
					}
					else
					{
						hasMore = false; // 请求行解析错误，停止解析
					}
				}
				else
				{
					hasMore = false; // 请求行不完整，等待更多数据
				}
			}
			else if (state_ == kExpectHeaders) // 当前状态：期望解析请求头
			{
				const char* crlf = buf->findCRLF(); // 查找 CRLF，表示一行请求头结束
				if (crlf)
				{
					const char* colon = std::find(buf->peek(), crlf, ':'); // 查找冒号，分隔键和值
					if (colon < crlf) // 找到了冒号，解析一行头部
					{
						request_.addHeader(buf->peek(), colon, crlf); // 将头部键值对添加到 HttpRequest 对象中
					}
					else if (buf->peek() == crlf)
					{
						// 空行，表示请求头解析完毕
						// 根据请求方法和 Content-Length 判断是否需要继续读取请求体
						if (request_.method() == HttpRequest::kPost ||
							request_.method() == HttpRequest::kPut)
						{
							std::string contentLength = request_.getHeader("Content-Length"); // 获取 Content-Length
							if (!contentLength.empty())
							{
								request_.setContentLength(std::stoi(contentLength)); // 设置请求体长度
								if (request_.contentLength() > 0)
								{
									state_ = kExpectBody; // 有请求体，切换状态为解析请求体
								}
								else
								{
									state_ = kGotAll; // Content-Length 为0，解析完成
									hasMore = false;
								}
							}
							else
							{
								// POST/PUT 请求没有 Content-Length，HTTP 请求语法错误
								ok = false;
								hasMore = false;
							}
						}
						else
						{
							// GET/HEAD/DELETE 等方法通常没有请求体，解析完成
							state_ = kGotAll;
							hasMore = false;
						}
					}
					else
					{
						ok = false; // 请求头行格式错误（没有冒号且不是空行）
						hasMore = false;
					}
					buf->retrieveUntil(crlf + 2); // 移动读指针到下一行数据
				}
				else
				{
					hasMore = false; // 请求头不完整，等待更多数据
				}
			}
			else if (state_ == kExpectBody) // 当前状态：期望解析请求体
			{
				// 检查缓冲区中是否有足够的数据
				if (buf->readableBytes() < request_.contentLength())
				{
					hasMore = false; // 数据不完整，等待更多数据
					return true; // 返回 true，表示解析暂时成功但还未完成
				}

				// 读取请求体内容，只读取 Content-Length 指定的长度
				std::string body(buf->peek(), buf->peek() + request_.contentLength());
				request_.setBody(body); // 设置 HttpRequest 对象的请求体

				buf->retrieve(request_.contentLength()); // 移动读指针，准确跳过请求体长度

				state_ = kGotAll; // 请求解析完成
				hasMore = false;
			}
		}
		return ok; // 返回解析结果，false 表示报文语法解析错误
	}

	// 解析请求行
	bool HttpContext::processRequestLine(const char* begin, const char* end)
	{
		bool succeed = false; // 标志解析是否成功
		const char* start = begin;
		const char* space = std::find(start, end, ' '); // 查找第一个空格，分隔方法和路径
		if (space != end && request_.setMethod(start, space)) // 设置方法成功
		{
			start = space + 1; // 移动到路径开始
			space = std::find(start, end, ' '); // 查找第二个空格，分隔路径和 HTTP 版本
			if (space != end)
			{
				const char* argumentStart = std::find(start, space, '?'); // 查找路径中的参数 '?'
				if (argumentStart != space) // 请求带参数
				{
					request_.setPath(start, argumentStart); // 设置路径
					request_.setQueryParameters(argumentStart + 1, space); // 设置查询参数
				}
				else // 请求不带参数
				{
					request_.setPath(start, space); // 设置路径
				}

				start = space + 1; // 移动到 HTTP 版本部分
				succeed = ((end - start == 8) && std::equal(start, end - 1, "HTTP/1.")); // 验证版本前缀是否正确
				if (succeed)
				{
					if (*(end - 1) == '1')
					{
						request_.setVersion("HTTP/1.1"); // 设置 HTTP/1.1
					}
					else if (*(end - 1) == '0')
					{
						request_.setVersion("HTTP/1.0"); // 设置 HTTP/1.0
					}
					else
					{
						succeed = false; // 版本号不合法
					}
				}
			}
		}
		return succeed; // 返回请求行解析结果
	}


} // namespace http