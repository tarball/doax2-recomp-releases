// The guide's system UIs (XamShow*UI). The runtime shows none: each is answered the way a player who backs out, or keeps
// the defaults, would answer it, with the notifications a real one sends around it: XN_SYS_UI 1 as it opens, then XN_SYS_UI
// 0 as it closes. Games pause for the guide on XN_SYS_UI, and DOAX2's XBOX LIVE menu waits for the sign-in UI to open and
// close before it checks the sign-in state again.
//
// Everything happens at the game's own pace, so that two machines in a netplay session go through it alike (DOA4's
// lockstep Versus menus show player 2's sign-in prompt on both): the answer comes at once (an XOVERLAPPED completes
// before the call returns), and a UI closes for each listener on its first poll after the opening that finds nothing else
// to give (xam/misc.cpp), rather than after a time.
#include "crash_handler.h"
#include "guest_abi.h"
#include "kernel/objects.h"
#include "log.h"
#include "xam/xam.h"

#include <string>

namespace
{
    // A big-endian UTF-16 guest string as UTF-8 for the log (non-ASCII characters become '?').
    std::string GuestWideString(const be<uint16_t>* text)
    {
        std::string result;
        for (size_t i = 0; text && text[i] != 0 && i < 4096; i++)
        {
            const uint16_t c = text[i];
            result += c == 0x0A || (c >= 0x20 && c < 0x7F) ? char(c) : '?';
        }
        return result;
    }

    std::string GuestWideString(uint32_t address)
    {
        return address ? GuestWideString(reinterpret_cast<const be<uint16_t>*>(guest::ToHost(address))) : std::string();
    }

    // XShowSigninUI(users, flags): the player backs out. The profile stays as it is: one local profile, never on Xbox Live,
    // and nobody else signs in. (DOA4 asks player 2 to sign in with a message box first, which a player who isn't signed
    // in declines, so it doesn't get here.)
    uint32_t XamShowSigninUI(uint32_t usersNeeded, uint32_t flags)
    {
        LOG_INFO("XamShowSigninUI(%u users, flags 0x%X): backed out, the profile stays as it is", usersNeeded, flags);
        return xam::ShowSystemUi("sign-in", [] { return xam::kErrorSuccess; }, nullptr);
    }

    // The virtual keyboard: answered with its default text, as by a player who presses Done.
    uint32_t XamShowKeyboardUI(uint32_t userIndex, uint32_t flags, const be<uint16_t>* defaultText, const be<uint16_t>* title,
        const be<uint16_t>* description, be<uint16_t>* result, uint32_t resultLength, xam::Overlapped* overlapped)
    {
        if (!result || resultLength == 0)
            return xam::Complete(overlapped, xam::kErrorInvalidParameter);
        LOG_INFO("XamShowKeyboardUI (user %u, flags 0x%X): \"%s\" (\"%s\"): answering the default text \"%s\"", userIndex, flags,
            GuestWideString(title).c_str(), GuestWideString(description).c_str(), GuestWideString(defaultText).c_str());
        return xam::ShowSystemUi("keyboard", [=] {
            uint32_t length = 0;
            for (; defaultText && length + 1 < resultLength && defaultText[length] != 0; length++)
                result[length] = defaultText[length];
            result[length] = 0;
            return xam::kErrorSuccess;
        }, overlapped);
    }

    // System message boxes are answered with the focused button (logged for the record), or for a player who isn't signed
    // in with the last, declining: only the one profile can sign in (XamShowSigninUI), so a player asked to sign in (DOA4's
    // player 2 joining a versus fight: [Sign In], [Don't Sign In]) would otherwise be asked again forever.
    uint32_t ShowMessageBox(uint32_t userIndex, const be<uint16_t>* title, const be<uint16_t>* text, uint32_t buttonCount,
        const be<uint32_t>* buttons, uint32_t focusButton, uint32_t flags, be<uint32_t>* result, xam::Overlapped* overlapped)
    {
        uint32_t answer = focusButton < buttonCount ? focusButton : 0;
        if (userIndex < 4 && !xam::IsUserSignedIn(userIndex) && buttonCount > 1)
            answer = buttonCount - 1;
        std::string buttonText;
        for (uint32_t i = 0; buttons && i < buttonCount && i < 3; i++)
            buttonText += std::string(i ? ", " : "") + "[" + GuestWideString(buttons[i]) + "]";
        LOG_WARN("XamShowMessageBoxUI (user %u, flags 0x%X): \"%s\": \"%s\" buttons %s; answering %u", userIndex, flags, GuestWideString(title).c_str(),
            GuestWideString(text).c_str(), buttonText.c_str(), answer);
        return xam::ShowSystemUi("message box", [=] {
            if (result)
                *result = answer;
            return xam::kErrorSuccess;
        }, overlapped);
    }

    uint32_t XamShowMessageBoxUI(uint32_t userIndex, const be<uint16_t>* title, const be<uint16_t>* text, uint32_t buttonCount,
        const be<uint32_t>* buttons, uint32_t focusButton, uint32_t flags, be<uint32_t>* result, xam::Overlapped* overlapped)
    {
        return ShowMessageBox(userIndex, title, text, buttonCount, buttons, focusButton, flags, result, overlapped);
    }

    // XAPI's own message boxes (DOAX2's display check) use the Ex form, which takes one more argument before the result.
    uint32_t XamShowMessageBoxUIEx(uint32_t userIndex, const be<uint16_t>* title, const be<uint16_t>* text, uint32_t buttonCount,
        const be<uint32_t>* buttons, uint32_t focusButton, uint32_t flags, uint32_t /*unknown*/, be<uint32_t>* result, xam::Overlapped* overlapped)
    {
        return ShowMessageBox(userIndex, title, text, buttonCount, buttons, focusButton, flags, result, overlapped);
    }

    // XShowMarketplaceUI(user, entry point, offer, content categories) passes an extra -1 before the categories and a 0
    // after them. The marketplace needs Xbox Live: the player backs out.
    uint32_t XamShowMarketplaceUI(uint32_t userIndex, uint32_t entryPoint, uint64_t offerId, uint32_t /*unknown*/, uint32_t contentCategories,
        uint32_t /*unknown2*/)
    {
        LOG_INFO("XamShowMarketplaceUI(user %u, entry point %u, offer %016llX, categories 0x%X): no Xbox Live, backed out", userIndex, entryPoint,
            (unsigned long long)offerId, contentCategories);
        return xam::ShowSystemUi("marketplace", [] { return xam::kErrorSuccess; }, nullptr);
    }

    // A player's gamer card (XShowGamerCardUI): there's nothing to show offline, so it closes at once.
    uint32_t XamShowGamerCardUIForXUID(uint32_t userIndex, uint64_t xuid, uint32_t /*unknown*/)
    {
        LOG_INFO("XamShowGamerCardUIForXUID(user %u, %016llX): closed", userIndex, (unsigned long long)xuid);
        return xam::ShowSystemUi("gamer card", [] { return xam::kErrorSuccess; }, nullptr);
    }

    // A disc read failed. A console shows its unreadable disc message, which the player can only leave by turning it off.
    [[noreturn]] void XamShowDirtyDiscErrorUI(uint32_t userIndex)
    {
        LOG_ERROR("XamShowDirtyDiscErrorUI(user %u)", userIndex);
        crash::Fatal("The game couldn't read its disc, and can't go on (a console shows its unreadable disc message here).\n\n"
                     "The disc image may be damaged, or the runtime failed to read it: the log has the file it was reading.");
    }
}

namespace xam
{
    uint32_t ShowSystemUi(const char* name, std::function<uint32_t()> answer, Overlapped* overlapped)
    {
        // Only for a game that waits for them (CMakeLists.txt's GAME_SYSTEM_UI_NOTIFICATIONS).
        if (GAME_SYSTEM_UI_NOTIFICATIONS)
            BroadcastNotification(kXnSysUi, 1);
        const uint32_t result = answer();
        LOG_INFO("System UI (%s) answered%s", name, GAME_SYSTEM_UI_NOTIFICATIONS ? "; it closes on each listener's next poll" : "");
        return Complete(overlapped, result);
    }
}

GUEST_EXPORT(XamShowSigninUI, XamShowSigninUI)
GUEST_EXPORT(XamShowKeyboardUI, XamShowKeyboardUI)
GUEST_EXPORT(XamShowMessageBoxUI, XamShowMessageBoxUI)
GUEST_EXPORT(XamShowMessageBoxUIEx, XamShowMessageBoxUIEx)
GUEST_EXPORT(XamShowMarketplaceUI, XamShowMarketplaceUI)
GUEST_EXPORT(XamShowGamerCardUIForXUID, XamShowGamerCardUIForXUID)
GUEST_EXPORT(XamShowDirtyDiscErrorUI, XamShowDirtyDiscErrorUI)
