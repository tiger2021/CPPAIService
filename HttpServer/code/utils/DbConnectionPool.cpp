#include "DbConnectionPool.h"
#include "DbException.h"
#include <muduo/base/Logging.h>

namespace http 
{
namespace db 
{

void DbConnectionPool::init(const std::string& host,
                          const std::string& user,
                          const std::string& password,
                          const std::string& database,
                          size_t poolSize) 
{
    // 连接池会被多个线程访问，所以操作其成员变量时需要加锁
    std::lock_guard<std::mutex> lock(mutex_);
    // 确保只初始化一次
    if (initialized_) 
    {
        return;
    }

    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;

    // 创建连接
    for (size_t i = 0; i < poolSize; ++i) 
    {
        connections_.push(createConnection());
    }

    initialized_ = true;
    LOG_INFO << "Database connection pool initialized with " << poolSize << " connections";
}

DbConnectionPool::DbConnectionPool() 
{
    checkThread_ = std::thread(&DbConnectionPool::checkConnections, this);
    checkThread_.detach();
}

DbConnectionPool::~DbConnectionPool() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) 
    {
        connections_.pop();
    }
    LOG_INFO << "Database connection pool destroyed";
}

// 从数据库连接池中获取一个可用的数据库连接
std::shared_ptr<DbConnection> DbConnectionPool::getConnection()
{
	std::shared_ptr<DbConnection> conn; // 用于保存从连接池中取出的连接对象

	{
		// 使用 unique_lock 加锁，便于后续配合条件变量使用
		std::unique_lock<std::mutex> lock(mutex_);

		// 当连接池为空时，线程需要等待
		while (connections_.empty())
		{
			// 如果连接池尚未初始化，则直接抛出异常
			if (!initialized_)
			{
				throw DbException("Connection pool not initialized");
			}

			// 记录日志，提示当前线程正在等待可用连接
			LOG_INFO << "Waiting for available connection...";

			// 当前线程进入等待状态，并释放 mutex_
			// 当有连接被归还时，由 notify_one 唤醒
			cv_.wait(lock);
		}

		// 从连接池队列中取出一个连接
		conn = connections_.front();

		// 将该连接从队列中移除，表示该连接已被占用
		connections_.pop();
	} // 作用域结束，自动释放 mutex_ 锁

	try
	{
		// 在锁外检查连接状态，避免长时间占用互斥锁
		if (!conn->ping())
		{
			// 如果连接不可用，记录警告日志
			LOG_WARN << "Connection lost, attempting to reconnect...";

			// 尝试重新建立数据库连接
			conn->reconnect();
		}

		// 返回一个新的 shared_ptr
		// 注意：这里使用了“自定义删除器”，并不会真正释放连接
		return std::shared_ptr<DbConnection>(conn.get(),
			// 自定义删除器，在 shared_ptr 引用计数归零时被调用
			[this, conn](DbConnection*) {

				// 加锁，保证对连接池操作的线程安全
				std::lock_guard<std::mutex> lock(mutex_);

				// 将连接重新放回连接池
				connections_.push(conn);

				// 唤醒一个正在等待连接的线程
				cv_.notify_one();
			});
	}
	catch (const std::exception& e)
	{
		// 捕获获取连接或重连过程中发生的异常
		LOG_ERROR << "Failed to get connection: " << e.what();

		{
			// 发生异常时，也需要保证连接被归还到连接池
			std::lock_guard<std::mutex> lock(mutex_);

			// 将连接重新放回连接池，避免连接泄漏
			connections_.push(conn);

			// 通知其他等待线程
			cv_.notify_one();
		}

		// 异常继续向上抛出，由上层调用者处理
		throw;
	}
}


std::shared_ptr<DbConnection> DbConnectionPool::createConnection() 
{
    return std::make_shared<DbConnection>(host_, user_, password_, database_);
}

// 修改检查连接的函数
void DbConnectionPool::checkConnections() 
{
    while (true) 
    {
        try 
        {
            std::vector<std::shared_ptr<DbConnection>> connsToCheck;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (connections_.empty()) 
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                
                auto temp = connections_;
                while (!temp.empty()) 
                {
                    connsToCheck.push_back(temp.front());
                    temp.pop();
                }
            }
            
            // 在锁外检查连接
            for (auto& conn : connsToCheck) 
            {
                if (!conn->ping()) 
                {
                    try 
                    {
                        conn->reconnect();
                    } 
                    catch (const std::exception& e) 
                    {
                        LOG_ERROR << "Failed to reconnect: " << e.what();
                    }
                }
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(60));
        } 
        catch (const std::exception& e) 
        {
            LOG_ERROR << "Error in check thread: " << e.what();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

} // namespace db
} // namespace http