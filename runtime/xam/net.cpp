// NetDll exports for an offline console: the stack starts, but there is no network to use.
// Every NetDll call takes the caller type (title or system) as its first argument.
// Also Xbox Live sessions (XamSession*).
#include "guest_abi.h"
#include "kernel/objects.h"
#include "log.h"
#include "xam/xam.h"

#include <cstring>
#include <random>

namespace
{
    constexpr uint32_t kSocketError = 0xFFFFFFFF;
    constexpr uint32_t kWsaENetDown = 10050;
    constexpr uint32_t kXnAddrStatic = 0x04;

    thread_local uint32_t t_lastError = 0;

    uint32_t Fail(uint32_t error)
    {
        t_lastError = error;
        return kSocketError;
    }

    uint32_t NetDll_XNetStartup(uint32_t /*caller*/, uint32_t /*params*/) { return 0; }

    struct WsaData
    {
        be<uint16_t> version;
        be<uint16_t> highVersion;
        char description[257];
        char systemStatus[129];
        be<uint16_t> maxSockets;
        be<uint16_t> maxUdpDatagram;
        be<uint32_t> vendorInfo;
    };

    uint32_t NetDll_WSAStartup(uint32_t /*caller*/, uint32_t version, WsaData* data)
    {
        if (data)
        {
            memset(data, 0, sizeof(*data));
            data->version = uint16_t(version);
            data->highVersion = uint16_t(version);
        }
        return 0;
    }

    uint32_t NetDll_WSAGetLastError() { return t_lastError; }

    struct XnAddr
    {
        be<uint32_t> ina;
        be<uint32_t> inaOnline;
        be<uint16_t> portOnline;
        uint8_t enet[6];
        uint8_t online[20];
    };
    static_assert(sizeof(XnAddr) == 36);

    // A loopback address with a fixed, non-zero MAC (some network libraries reject an all-zero one).
    uint32_t NetDll_XNetGetTitleXnAddr(uint32_t /*caller*/, XnAddr* address)
    {
        if (address)
        {
            memset(address, 0, sizeof(*address));
            address->ina = 0x7F000001;
            const uint8_t mac[6] = { 0x00, 0x22, 0x48, 0x00, 0xD0, 0xA4 };
            memcpy(address->enet, mac, sizeof(mac));
        }
        return kXnAddrStatic;
    }

    uint32_t NetDll_XNetRandom(uint32_t /*caller*/, uint8_t* buffer, uint32_t length)
    {
        static thread_local std::mt19937 generator{ std::random_device{}() };
        for (uint32_t i = 0; buffer && i < length; i++)
            buffer[i] = uint8_t(generator());
        return 0;
    }

    uint32_t NetDll_socket(uint32_t /*caller*/, uint32_t af, uint32_t type, uint32_t protocol)
    {
        LOG_WARN("socket(%u, %u, %u): no network", af, type, protocol);
        return Fail(kWsaENetDown);
    }

    uint32_t NetDll_closesocket(uint32_t /*caller*/, uint32_t /*socket*/) { return 0; }
    uint32_t NetDll_bind(uint32_t, uint32_t, uint32_t, uint32_t) { return Fail(kWsaENetDown); }
    uint32_t NetDll_ioctlsocket(uint32_t, uint32_t, uint32_t, uint32_t) { return Fail(kWsaENetDown); }
    uint32_t NetDll_sendto(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return Fail(kWsaENetDown); }
    uint32_t NetDll_recvfrom(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return Fail(kWsaENetDown); }

    // QoS and secure-address calls fail like they do with no network.
    uint32_t NetDll_XNetQosListen(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kWsaENetDown; }
    uint32_t NetDll_XNetQosLookup(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kWsaENetDown; }
    uint32_t NetDll_XNetQosRelease(uint32_t, uint32_t) { return 0; }
    uint32_t NetDll_XNetInAddrToXnAddr(uint32_t, uint32_t, uint32_t, uint32_t) { return kWsaENetDown; }
    uint32_t NetDll_XNetXnAddrToInAddr(uint32_t, uint32_t, uint32_t, uint32_t) { return kWsaENetDown; }

    uint32_t XNetLogonGetTitleID() { return xam::kTitleId; }

    // Xbox Live sessions. XSessionCreate makes a session object with XamSessionCreateHandle, takes a reference to it and
    // passes it to XAM's Live app (XMsg 0xFB message 0xB0010), which makes no sessions offline (messages.cpp): the
    // creation fails there, and XSessionCreate closes the handle.
    class Session : public kernel::GuestBackedObject
    {
    public:
        Session() : GuestBackedObject(sizeof(kernel::DispatcherHeader))
        {
            Header()->type = uint8_t(kernel::DispatcherType::NotificationEvent);
            Header()->size = sizeof(kernel::DispatcherHeader) / 4;
        }
        const char* TypeName() const override { return "Session"; }
    };

    uint32_t XamSessionCreateHandle(be<uint32_t>* handle)
    {
        if (!handle)
            return xam::kErrorInvalidParameter;
        auto session = std::make_shared<Session>();
        kernel::RegisterGuestObject(session);
        *handle = kernel::CreateHandle(session);
        LOG_INFO("XamSessionCreateHandle: 0x%08X", uint32_t(*handle));
        return xam::kErrorSuccess;
    }

    // The session's object, with a reference ObDereferenceObject releases.
    uint32_t XamSessionRefObjByHandle(uint32_t handle, be<uint32_t>* object)
    {
        if (!kernel::LookupHandleAs<Session>(handle))
            return 6; // ERROR_INVALID_HANDLE
        const uint32_t guestObject = kernel::ReferenceObject(handle);
        if (object)
            *object = guestObject;
        return xam::kErrorSuccess;
    }
}

GUEST_EXPORT(NetDll_XNetStartup, NetDll_XNetStartup)
GUEST_EXPORT(NetDll_WSAStartup, NetDll_WSAStartup)
GUEST_EXPORT(NetDll_WSAGetLastError, NetDll_WSAGetLastError)
GUEST_EXPORT(NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr)
GUEST_EXPORT(NetDll_XNetRandom, NetDll_XNetRandom)
GUEST_EXPORT(NetDll_socket, NetDll_socket)
GUEST_EXPORT(NetDll_closesocket, NetDll_closesocket)
GUEST_EXPORT(NetDll_bind, NetDll_bind)
GUEST_EXPORT(NetDll_ioctlsocket, NetDll_ioctlsocket)
GUEST_EXPORT(NetDll_sendto, NetDll_sendto)
GUEST_EXPORT(NetDll_recvfrom, NetDll_recvfrom)
GUEST_EXPORT(NetDll_XNetQosListen, NetDll_XNetQosListen)
GUEST_EXPORT(NetDll_XNetQosLookup, NetDll_XNetQosLookup)
GUEST_EXPORT(NetDll_XNetQosRelease, NetDll_XNetQosRelease)
GUEST_EXPORT(NetDll_XNetInAddrToXnAddr, NetDll_XNetInAddrToXnAddr)
GUEST_EXPORT(NetDll_XNetXnAddrToInAddr, NetDll_XNetXnAddrToInAddr)
GUEST_EXPORT(XNetLogonGetTitleID, XNetLogonGetTitleID)
GUEST_EXPORT(XamSessionCreateHandle, XamSessionCreateHandle)
GUEST_EXPORT(XamSessionRefObjByHandle, XamSessionRefObjByHandle)
