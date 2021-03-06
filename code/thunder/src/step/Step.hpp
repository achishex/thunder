/*******************************************************************************
 * Project:  AsyncServer
 * @file     Step.hpp
 * @brief    异步步骤基类
 * @author   cjy
 * @date:    2017年7月27日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef STEP_HPP_
#define STEP_HPP_

#include <map>
#include <set>

#include "hiredis/async.h"
#include "hiredis/hiredis.h"
#include "libev/ev.h"         // need ev_tstamp
#include "log4cplus/fileappender.h"
#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"

#include "../ThunderDefine.hpp"
#include "../ThunderError.hpp"
#include "labor/NodeLabor.hpp"
#include "cmd/CW.hpp"
#include "protocol/msg.pb.h"
#include "session/Session.hpp"


namespace thunder
{

class NodeWorker;
class Cmd;
class RedisStep;

/**
 * @brief 步骤基类
 * @note 全异步Server框架基于状态机设计模式实现，Step类就是框架的状态机基类。Step类还保存了业务层连接标
 * 识到连接层实际连接的对应关系，业务层通过Step类可以很方便地把数据发送到指定连接。
 */
class Step
{
public:
    Step(Step* pNextStep = NULL);
    Step(const MsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody,
                    Step* pNextStep = NULL);
    Step(const MsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, Step* pNextStep = NULL);
    virtual ~Step();

    /**
     * @brief 提交，发出
     * @note 注册了一个回调步骤之后执行Emit()就开始等待回调。 Emit()大部分情况下不需要传递参数，
     * 三个带缺省值的参数是为了让上一个通用Step执行出错时将错误码带入下一步业务逻辑Step，由具体
     * 业务逻辑处理。
     * @param iErrno  错误码
     * @param strErrMsg 错误信息
     * @param strErrShow 展示给用户的错误描述
     * @return 执行状态
     */
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "") = 0;

    /**
     * @brief 步骤回调函数
     * @note 满足某个条件，比如监控某个文件描述符fd的EPOLLIN事件和EPOLLERR事件，当这个fd的
     * 这两类事件中的任意一类到达时则会调用Callback()。具体使用到哪几个参数与业务逻辑有关，前三个
     * 参数的使用概率高。
     * @param stMsgShell 消息外壳，回调可通过消息外壳原路回复消息，若消息不是来源于网络IO，则
     * 消息外壳为空
     * @param oInMsgHead 消息头。
     * @param oInMsgBody 消息体。
     * @param data 数据指针，基本网络IO时为空，有专用数据时使用，比如redis的reply。
     */
    virtual E_CMD_STATUS Callback(
                    const MsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL) = 0;

    /**
     * @brief 步骤超时回调
     */
    virtual E_CMD_STATUS Timeout() = 0;

    /**
     * @brief 预设数据
     * @note 在Emit()执行前预设数据，通常Step所需数据从构造函数传入，SetData()提供了除构造函数
     * 之外的另一种数据传入方式。虽然如此，但尽量不要使用SetData()传入数据，只有当数据无法通过
     * 构造函数传入（构造时数据尚无法获得）时才使用SetData()传入数据。
     *     data作为Emit()的最右边一个参数传入（如Callback()函数）是最好的处理方式，但因已有大量
     * Step派生类未声明带data参数的Emit()，为免大量修改代码才折衷增加SetData()。
     * @param data 数据
     */
    virtual void SetData(void* data){};

public:
    /**
     * @brief 设置步骤最近刷新时间
     */
    void SetActiveTime(ev_tstamp dActiveTime)
    {
        m_dActiveTime = dActiveTime;
    }

    /**
     * @brief 获取步骤刷新时间
     */
    ev_tstamp GetActiveTime() const
    {
        return(m_dActiveTime);
    }

    /**
     * @brief 设置步骤超时时间
     */
    void SetTimeout(ev_tstamp dTimeout)
    {
        m_dTimeout = dTimeout;
    }

    /**
     * @brief 获取步骤超时时间
     */
    ev_tstamp GetTimeout() const
    {
        return(m_dTimeout);
    }

    /**
     * @brief 延迟超时时间
     */
    void DelayTimeout();

    const std::string& ClassName() const
    {
        return(m_strClassName);
    }

    void SetClassName(const std::string& strClassName)
    {
        m_strClassName = strClassName;
    }

    /**
     * @brief 是否注册了回调
     * @return 注册结果
     */
    bool IsRegistered() const
    {
        return(m_bRegistered);
    }
protected:
    /**
     * @brief 登记回调步骤
     * @note  登记回调步骤。如果StepA.Callback()调用之后仍有后续步骤，则需在StepA.Callback()
     * 中new一个 新的具体步骤thunder::Step子类实例StepB，调用thunder::Step基类的RegisterCallback()
     * 方法将该新实例登记并执行该新实例的StepB.Start()方法，若StepB.Start()执行成功则后续
     * StepB.Callback()会被调用，若StepB.Start()执行失败，则调用thunder::Step基类的
     * DeleteCallback()将刚登记的StepB删除掉并执行对应的错误处理。
     * 若RegisterCallback()登记失败（失败可能性微乎其微）则应将StepB销毁,重新new StepB实例并登记，
     * 若多次（可自定义）登记失败则应放弃登记，并将StepB销毁；若RegisterCallback()登记成功则一定不可以
     * 销毁StepB（销毁操作会自动在回调StepB.Callback()之后执行）
     * @param pStep 回调步骤实例
     * @param dTimeout 超时时间
     * @return 是否登记成功
     */
    bool RegisterCallback(Step* pStep, ev_tstamp dTimeout = 0.0);

    /**
     * @brief 删除回调步骤
     * @note 在RegisterCallback()成功，但执行pStep->Start()失败时被调用
     * @param pStep 回调步骤实例
     */
    void DeleteCallback(Step* pStep);

    /**
     * @brief 预处理
     * @note 预处理用于将等待预处理对象与框架建立弱联系，使被处理的对象可以获得框架一些工具，如写日志指针
     * @param pStep 等待预处理的Step
     * @return 预处理结果
     */
    bool Pretreat(Step* pStep);

    /**
     * @brief 登记会话
     * @param pSession 会话实例
     * @return 是否登记成功
     */
    bool RegisterCallback(Session* pSession);

    /**
     * @brief 删除回调步骤
     * @note 在RegisterCallback()成功，但执行pStep->Start()失败时被调用
     * @param pSession 会话实例
     */
    void DeleteCallback(Session* pSession);

    /**
     * @brief 获取工作目录
     * @return 工作目录
     */
    const std::string& GetWorkPath() const;

    /**
     * @brief 获取日志类实例
     * @note 派生类写日志时调用
     * @return 日志类实例
     */
    log4cplus::Logger GetLogger()
    {
        return (*m_pLogger);
    }

    log4cplus::Logger* GetLoggerPtr()
    {
        return (m_pLogger);
    }

    uint32 GetNodeId();
    uint32 GetWorkerIndex();

    /**
     * @brief 获取当前Worker进程标识符
     * @note 当前Worker进程标识符由 IP:port:worker_index组成，例如： 192.168.18.22:30001.2
     * @return 当前Worker进程标识符
     */
    const std::string& GetWorkerIdentify();

    /**
     * @brief 获取当前节点类型
     * @return 当前节点类型
     */
    const std::string& GetNodeType() const;

    /**
     * @brief 获取Server自定义配置
     * @return Server自定义配置
     */
    const llib::CJsonObject& GetCustomConf() const;

    /**
     * @brief 获取当前时间
     * @note 获取当前时间，比time(NULL)速度快消耗小，不过没有time(NULL)精准，如果对时间精度
     * 要求不是特别高，建议调用GetNowTime()替代time(NULL)
     * @return 当前时间
     */
    time_t GetNowTime() const;

    /**
     * @brief 获取框架层操作者实例
     * @note 获取框架层操作者实例，业务层Step派生类偶尔会用到此函数。调用此函数时请先从Step基类查找是否有
     * 可替代的方法，能不获取框架层操作者实例则尽量不要获取。
     * @return 框架层操作者实例
     */
    NodeLabor* GetLabor()
    {
        return(m_pLabor);
    }

    /**
     * @brief 获取会话实例
     * @param uiSessionId 会话ID
     * @return 会话实例（返回NULL表示不存在uiSessionId对应的会话实例）
     */
    Session* GetSession(uint64 uiSessionId, const std::string& strSessionClass = "thunder::Session");
    Session* GetSession(const std::string& strSessionId, const std::string& strSessionClass = "thunder::Session");

    /**
     * @brief 连接成功后发送
     * @note 当前Server往另一个Server发送数据而两Server之间没有可用连接时，框架层向对端发起连接（发起连接
     * 的过程是异步非阻塞的，connect()函数返回的时候并不知道连接是否成功），并将待发送数据存放于应用层待发
     * 送缓冲区。当连接成功时将待发送数据从应用层待发送缓冲区拷贝到应用层发送缓冲区并发送。此函数由框架层自
     * 动调用，业务逻辑层无须关注。
     * @param stMsgShell 消息外壳
     * @return 是否发送成功
     */
    bool SendTo(const MsgShell& stMsgShell);

    /**
     * @brief 发送数据
     * @note 使用指定连接将数据发送。如果能直接得知stMsgShell（比如刚从该连接接收到数据，欲回确认包），就
     * 应调用此函数发送。此函数是SendTo()函数中最高效的一个。
     * @param stMsgShell 消息外壳
     * @param oMsgHead 数据包头
     * @param oMsgBody 数据包体
     * @return 是否发送成功
     */
    bool SendTo(const MsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody);

    /**
     * @brief 发送数据
     * @note 指定连接标识符将数据发送。此函数先查找与strIdentify匹配的stMsgShell，如果找到就调用
     * SendTo(const MsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
     * 发送，如果未找到则调用SendToWithAutoConnect(const std::string& strIdentify,
     * const MsgHead& oMsgHead, const MsgBody& oMsgBody)连接后再发送。
     * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
     * @param oMsgHead 数据包头
     * @param oMsgBody 数据包体
     * @return 是否发送成功
     */
    bool SendTo(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody);

    /**
     * @brief 发送到下一个同一类型的节点
     * @note 发送到下一个同一类型的节点，适用于对同一类型节点做轮询方式发送以达到简单的负载均衡。
     * @param strNodeType 节点类型
     * @param oMsgHead 数据包头
     * @param oMsgBody 数据包体
     * @return 是否发送成功
     */
    bool SendToNext(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody);

    /**
     * @brief 以取模方式选择发送到同一类型节点
     * @note 以取模方式选择发送到同一类型节点，实现简单有要求的负载均衡。
     * @param strNodeType 节点类型
     * @param uiModFactor 取模因子
     * @param oMsgHead 数据包头
     * @param oMsgBody 数据包体
     * @return 是否发送成功
     */
    bool SendToWithMod(const std::string& strNodeType, unsigned int uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody);


    //接入服务器使用的对外接口
    bool SendToClient(const MsgShell& stMsgShell,MsgHead& oMsgHead,const google::protobuf::Message &message,
                    const std::string& additional = "",uint64 sessionid = 0,const std::string& stressionid = "")
    {
        return GetLabor()->SendToClient(stMsgShell,oMsgHead,message,additional,sessionid,stressionid);
    }
    bool SendToClient(const std::string& strIdentify,MsgHead& oMsgHead,const google::protobuf::Message &message,
                    const std::string& additional = "",uint64 sessionid = 0,const std::string& stressionid = "")
    {
        return GetLabor()->SendToClient(strIdentify,oMsgHead,message,additional,sessionid,stressionid);
    }
    bool ParseFromMsg(const MsgBody& oInMsgBody,google::protobuf::Message &message)
    {
        return GetLabor()->ParseFromMsg(oInMsgBody,message);
    }
    //发送异步step，step对象内存由worker管理
    bool AsyncStep(Step* pStep,ev_tstamp dTimeout = 0.0);
    bool AsyncNextStep(Step* pStep,ev_tstamp dTimeout = 0.0);

    bool EmitStorageAccess(const std::string &strMsgSerial,StorageCallbackStep callback,
			const std::string &nodeType="PROXY",uint32 uiCmd = thunder::CMD_REQ_STORATE);

    bool EmitStandardAccess(const std::string &strMsgSerial,StandardCallbackStep callback,
			const std::string &nodeType,uint32 uiCmd);
    /**
     * @brief 延迟下一个步骤的超时时间
     */
    void DelayNextStep();

    /**
     * @brief 执行下一步
     * @param pNextStep 下一个步骤
     * @return 执行结果
     */
    bool NextStep(Step* pNextStep, int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrClientShow = "");
    bool NextStep(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrClientShow = "");

    thunder::Step* GetNextStep()
    {
        return(m_pNextStep);
    }

    void SetNextStepNull();

public:
    /**
     * @brief 获取当前Step的序列号
     * @note 获取当前Step的序列号，对于同一个Step实例，每次调用GetSequence()获取的序列号是相同的。
     * @return 序列号
     */
    uint32 GetSequence();

    /**
     * @brief 添加指定标识的消息外壳
     * @note 添加指定标识的消息外壳由Cmd类实例和Step类实例调用，该调用会在Step类中添加一个标识
     * 和消息外壳的对应关系，同时Worker中的连接属性也会添加一个标识。
     * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
     * @param stMsgShell  消息外壳
     * @return 是否添加成功
     */
    bool AddMsgShell(const std::string& strIdentify, const MsgShell& stMsgShell);

    /**
     * @brief 删除指定标识的消息外壳
     * @note 删除指定标识的消息外壳由Worker类实例调用，在IoError或IoTimeout时调
     * 用。
     */
    void DelMsgShell(const std::string& strIdentify, const MsgShell& stMsgShell);

    /**
     * @brief 注册redis回调
     * @param strIdentify redis节点标识(192.168.16.22:9988形式的IP+端口)
     * @param pRedisStep redis步骤实例
     * @return 是否注册成功
     */
    bool RegisterCallback(const std::string& strIdentify, RedisStep* pRedisStep);

    /**
     * @brief 注册redis回调
     * @param strHost redis节点IP
     * @param iPort redis端口
     * @param pRedisStep redis步骤实例
     * @return 是否注册成功
     */
    bool RegisterCallback(const std::string& strHost, int iPort, RedisStep* pRedisStep);

    /**
     * @brief 添加标识的节点类型属性
     * @note 添加标识的节点类型属性，用于以轮询方式向同一节点类型的节点发送数据，以
     * 实现简单的负载均衡。只有Server间的各连接才具有NodeType属性，客户端到Access节
     * 点的连接不具有NodeType属性。
     * @param strNodeType 节点类型
     * @param strIdentify 连接标识符
     */
    void AddNodeIdentify(const std::string& strNodeType, const std::string& strIdentify);

    /**
     * @brief 删除标识的节点类型属性
     * @note 删除标识的节点类型属性，当一个节点下线，框架层会自动调用此函数删除标识
     * 的节点类型属性。
     * @param strNodeType 节点类型
     * @param strIdentify 连接标识符
     */
    void DelNodeIdentify(const std::string& strNodeType, const std::string& strIdentify);

    /*
     * @brief 添加Redis节点配置
     * @note 添加Redis节点配置函数由管理Redis节点配置的Cmd类实例调用，添加、删除节点均通过指定的Cmd类来完成。
     * @param strNodeType Redis节点类型
     * @param strHost 节点IP
     * @param iPort 节点端口
    void AddRedisNodeConf(const std::string strNodeType, const std::string strHost, int iPort);


     * @brief 删除Redis节点配置
     * @note 删除Redis节点配置函数由管理Redis节点配置的Cmd类实例调用，添加、删除节点均通过指定的Cmd类来完成。
     * @param strNodeType Redis节点类型
     * @param strHost 节点IP
     * @param iPort 节点端口
    void DelRedisNodeConf(const std::string strNodeType, const std::string strHost, int iPort);
     */

    /**
     * @brief 添加指定标识的redis context地址
     * @note 添加指定标识的redis context由Worker调用，该调用会在Step类中添加一个标识
     * 和redis context的对应关系。
     */
    bool AddRedisContextAddr(const std::string& strHost, int iPort, redisAsyncContext* ctx);

    /**
     * @brief 删除指定标识的redis context地址
     * @note 删除指定标识的到redis地址的对应关系（此函数被调用时，redis context的资源已被释放或将被释放）
     * 用。
     */
    void DelRedisContextAddr(const redisAsyncContext* ctx);

    /**
     * @brief 添加上一步骤
     * @note 用于设置步骤间的依赖关系，框架在当前步骤终止并销毁前检查是否仍有以当前步骤为下一步的步骤尚未
     * 结束，若有则延长当前步骤的生命期。
     * @param pStep 上一步骤
     */
    void AddPreStepSeq(Step* pStep);

private:
    /**
     * @brief 设置框架层操作者
     * @note 设置框架层操作者，由框架层调用，业务层派生类可直接忽略此函数
     * @param pLabor 框架层操作者，一般为ThunderWorker的实例
     */
    void SetLabor(NodeLabor* pLabor)
    {
        m_pLabor = pLabor;
    }

    /**
     * @brief 设置日志类实例
     * @note 设置框架层操作者，由框架层调用，业务层派生类可直接忽略此函数
     * @param logger 日志类实例
     */
    void SetLogger(log4cplus::Logger* pLogger)
    {
        m_pLogger = pLogger;
    }

    /**
     * @brief 设置为已注册状态
     */
    void SetRegistered()
    {
        m_bRegistered = true;
    }

    /**
     * @brief 设置为未注册状态
     * @note 当且仅当UnRegisterCallback(Step*)时由框架调用
     */
    void UnsetRegistered()
    {
        m_bRegistered = false;
    }

    void AddNextStepSeq(Step* pStep);

protected:  // 请求端的上下文信息，通过Step构造函数初始化，若调用的是不带参数的构造函数Step()，则这几个成员不会被初始化
    MsgShell m_stReqMsgShell;
    MsgHead m_oReqMsgHead;
    MsgBody m_oReqMsgBody;

private:
    bool m_bRegistered;
    uint32 m_ulSequence;
    ev_tstamp m_dActiveTime;
    ev_tstamp m_dTimeout;
    std::string m_strWorkerIdentify;
    NodeLabor* m_pLabor;
    log4cplus::Logger* m_pLogger;
    ev_timer* m_pTimeoutWatcher;
    std::string m_strClassName;
    Step* m_pNextStep;
    std::set<uint32> m_setNextStepSeq;
    std::set<uint32> m_setPreStepSeq;

    friend class NodeWorker;
	thunder::uint32 m_uiUserId;
	thunder::uint32 m_uiCmd;
};

} /* namespace thunder */

#endif /* STEP_HPP_ */
