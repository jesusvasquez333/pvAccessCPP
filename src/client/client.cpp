/*
 * Copyright information and license terms for this software can be
 * found in the file LICENSE that is included with the distribution
 */

#include <epicsMutex.h>
#include <epicsGuard.h>

#include <pv/pvData.h>
#include <pv/bitSet.h>
#include <pv/reftrack.h>

#define epicsExportSharedSymbols
#include "pv/logger.h"
#include "clientpvt.h"
#include "pv/pvAccess.h"
#include "pv/configuration.h"

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

typedef epicsGuard<epicsMutex> Guard;

namespace pvac {

Timeout::Timeout()
    :std::runtime_error("Timeout")
{}

struct ClientChannel::Impl : public pva::ChannelRequester,
                             public pvac::detail::wrapped_shared_from_this<ClientChannel::Impl>
{
    epicsMutex mutex;
    pva::Channel::shared_pointer channel;
    // assume few listeners per channel, store in vector
    typedef std::vector<ClientChannel::ConnectCallback*> listeners_t;
    listeners_t listeners;

    static size_t num_instances;

    Impl() {REFTRACE_INCREMENT(num_instances);}
    virtual ~Impl() {REFTRACE_DECREMENT(num_instances);}

    // called automatically via wrapped_shared_from_this
    void cancel()
    {
        // ClientChannel destroy implicitly removes all callbacks,
        // but doesn't destroy the Channel or cancel Operations
        Guard G(mutex);
        listeners.clear();
    }

    virtual std::string getRequesterName() OVERRIDE FINAL { return "ClientChannel::Impl"; }

    virtual void channelCreated(const pvd::Status& status, pva::Channel::shared_pointer const & channel) OVERRIDE FINAL {}

    virtual void channelStateChange(pva::Channel::shared_pointer const & channel, pva::Channel::ConnectionState connectionState) OVERRIDE FINAL
    {
        listeners_t notify;
        {
            Guard G(mutex);
            notify = listeners; // copy vector
        }
        ConnectEvent evt;
        evt.connected = connectionState==pva::Channel::CONNECTED;
        for(listeners_t::const_iterator it=notify.begin(), end=notify.end(); it!=end; ++it)
        {
            try {
                (*it)->connectEvent(evt);
            }catch(std::exception& e){
                LOG(pva::logLevelError, "Unhandled exception in connection state listener: %s\n", e.what());

                Guard G(mutex);
                for(listeners_t::iterator it2=listeners.begin(), end2=listeners.end(); it2!=end2; ++it2) {
                    if(*it==*it2) {
                        listeners.erase(it2);
                        break;
                    }
                }
            }
        }
    }
};

size_t ClientChannel::Impl::num_instances;

ClientChannel::Options::Options()
    :priority(0)
    ,address()
{}

bool ClientChannel::Options::operator<(const Options& O) const
{
    return priority<O.priority || (priority==O.priority && address<O.address);
}

Operation::Operation(const std::tr1::shared_ptr<Impl>& i)
    :impl(i)
{}

Operation::~Operation() {}

std::string Operation::name() const
{
    return impl ? impl->name() : "<NULL>";
}

void Operation::cancel()
{
    if(impl) impl->cancel();
}

ClientChannel::ClientChannel(const std::tr1::shared_ptr<pva::ChannelProvider>& provider,
                  const std::string& name,
                  const Options& opt)
    :impl(Impl::build())
{
    if(name.empty())
        THROW_EXCEPTION2(std::logic_error, "empty channel name not allowed");
    if(!provider)
        THROW_EXCEPTION2(std::logic_error, "NULL ChannelProvider");
    impl->channel = provider->createChannel(name, impl->internal_shared_from_this(),
                                            opt.priority, opt.address);
    if(!impl->channel)
        throw std::runtime_error("ChannelProvider failed to create Channel");
}

ClientChannel::~ClientChannel() {}

std::string ClientChannel::name() const
{
    return impl ? impl->channel->getChannelName() : std::string();
}

void ClientChannel::addConnectListener(ConnectCallback* cb)
{
    if(!impl) throw std::logic_error("Dead Channel");
    ConnectEvent evt;
    {
        Guard G(impl->mutex);

        for(Impl::listeners_t::const_iterator it=impl->listeners.begin(), end=impl->listeners.end(); it!=end; ++it)
        {
            if(cb==*it) return; // no duplicates
        }
        impl->listeners.push_back(cb);
        evt.connected = impl->channel->isConnected();
    }
    try{
        cb->connectEvent(evt);
    }catch(...){
        removeConnectListener(cb);
        throw;
    }
}

void ClientChannel::removeConnectListener(ConnectCallback* cb)
{
    if(!impl) throw std::logic_error("Dead Channel");
    Guard G(impl->mutex);

    for(Impl::listeners_t::iterator it=impl->listeners.begin(), end=impl->listeners.end(); it!=end; ++it)
    {
        if(cb==*it) {
            impl->listeners.erase(it);
            return;
        }
    }
}

static
void register_reftrack()
{
    static volatile int done;
    if(done) return;
    done = 1;
    // done is an optimization, duplicate calls to registerRef* are no-ops
    pvac::detail::registerRefTrack();
    pvac::detail::registerRefTrackGet();
    pvac::detail::registerRefTrackMonitor();
    pvac::detail::registerRefTrackRPC();
}

std::tr1::shared_ptr<epics::pvAccess::Channel>
ClientChannel::getChannel()
{ return impl->channel; }

struct ClientProvider::Impl
{
    static size_t num_instances;
    Impl() {register_reftrack(); REFTRACE_INCREMENT(num_instances);}
    ~Impl() {REFTRACE_DECREMENT(num_instances);}

    pva::ChannelProvider::shared_pointer provider;

    epicsMutex mutex;
    typedef std::map<std::pair<std::string, ClientChannel::Options>, std::tr1::weak_ptr<ClientChannel::Impl> > channels_t;
    channels_t channels;
};

size_t ClientProvider::Impl::num_instances;

ClientProvider::ClientProvider(const std::string& providerName,
                                       const std::tr1::shared_ptr<epics::pvAccess::Configuration>& conf)
    :impl(new Impl)
{
    std::string name;
    pva::ChannelProviderRegistry::shared_pointer reg;

    if(strncmp("server:", providerName.c_str(), 7)==0) {
        name = providerName.substr(7);
        reg = pva::ChannelProviderRegistry::servers();
    } else if(strncmp("client:", providerName.c_str(), 7)==0) {
        name = providerName.substr(7);
        reg = pva::ChannelProviderRegistry::clients();
    } else {
        name = providerName;
        reg = pva::ChannelProviderRegistry::clients();
    }
    impl->provider = reg->createProvider(name,
                                         conf ? conf : pva::ConfigurationBuilder()
                                                .push_env()
                                                .build());

    if(!impl->provider)
        THROW_EXCEPTION2(std::invalid_argument, providerName);
}

ClientProvider::ClientProvider(const std::tr1::shared_ptr<epics::pvAccess::ChannelProvider>& provider)
    :impl(new Impl)
{
    impl->provider = provider;

    if(!impl->provider)
        THROW_EXCEPTION2(std::invalid_argument, "null ChannelProvider");
}

ClientProvider::~ClientProvider() {}

ClientChannel
ClientProvider::connect(const std::string& name,
                            const ClientChannel::Options& conf)
{
    Guard G(impl->mutex);
    Impl::channels_t::key_type K(name, conf);
    Impl::channels_t::iterator it(impl->channels.find(K));
    if(it!=impl->channels.end()) {
        // cache hit
        std::tr1::shared_ptr<ClientChannel::Impl> chan(it->second.lock());
        if(chan)
            return ClientChannel(chan);
        else
            impl->channels.erase(it); // remove stale
    }
    // cache miss
    ClientChannel ret(impl->provider, name, conf);
    impl->channels[K] = ret.impl;
    return ret;
}

bool ClientProvider::disconnect(const std::string& name,
                                    const ClientChannel::Options& conf)
{
    Guard G(impl->mutex);

    Impl::channels_t::iterator it(impl->channels.find(std::make_pair(name, conf)));
    bool found = it!=impl->channels.end();
    if(found)
        impl->channels.erase(it);
    return found;
}

void ClientProvider::disconnect()
{
    Guard G(impl->mutex);
    impl->channels.clear();
}

namespace detail {

void registerRefTrack()
{
    epics::registerRefCounter("pvac::ClientChannel::Impl", &ClientChannel::Impl::num_instances);
    epics::registerRefCounter("pvac::ClientProvider::Impl", &ClientProvider::Impl::num_instances);
}

}

} //namespace pvac
