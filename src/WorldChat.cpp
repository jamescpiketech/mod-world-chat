#include "ScriptMgr.h"
#include "Player.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "Common.h"
#include "WorldSession.h"
#include "World.h"
#include "Config.h"
#include "ChatCommand.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>

using namespace Acore::ChatCommands;

struct WCConfig
{
    bool Enabled = true;
    std::string ChannelName = "Global";
    bool CrossFactions = true;
    bool Announce = true;
};

static WCConfig WC_Config;

static inline std::string ToUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
    return s;
}

static bool SameTeamOrCross(Player* from, Player* to)
{
    if (!from || !to)
        return false;
    if (WC_Config.CrossFactions)
        return true;
    return from->GetTeamId() == to->GetTeamId();
}

static inline char const* FactionTag(Player* p)
{
    return (p->GetTeamId() == TEAM_ALLIANCE)
        ? "|cff3399FF[A]|r "
        : "|cffff0000[H]|r ";
}

static inline char const* FactionTagPlain(Player* p)
{
    return (p->GetTeamId() == TEAM_ALLIANCE) ? "[A] " : "[H] ";
}

static inline char const* FactionColor(Player* p)
{
    return (p->GetTeamId() == TEAM_ALLIANCE)
        ? "|cff3399FF"
        : "|cffff0000";
}

static inline char const* ClassColor(Player* p)
{
    switch (p->getClass())
    {
        case CLASS_WARRIOR:      return "|cffC79C6E";
        case CLASS_PALADIN:      return "|cffF58CBA";
        case CLASS_HUNTER:       return "|cffABD473";
        case CLASS_ROGUE:        return "|cffFFF569";
        case CLASS_PRIEST:       return "|cffFFFFFF";
        case CLASS_DEATH_KNIGHT: return "|cffC41E3A";
        case CLASS_SHAMAN:       return "|cff0070DE";
        case CLASS_MAGE:         return "|cff69CCF0";
        case CLASS_WARLOCK:      return "|cff9482C9";
        case CLASS_DRUID:        return "|cffFF7D0A";
        default:                 return "|cffffffff";
    }
}

static void SendWorldMessage(Player* sender, std::string const& text)
{
    if (!WC_Config.Enabled || !sender || !sender->GetSession())
        return;

    // Colored line for in-game
    std::string coloredLine = std::string("|cffffd000[") + WC_Config.ChannelName + "]|r "
        + FactionTag(sender)
        + ClassColor(sender) + sender->GetName() + "|r: "
        + FactionColor(sender) + text + "|r";

    // Plaintext line for console/log
    std::string plainLine = std::string("[") + WC_Config.ChannelName + "] "
        + FactionTagPlain(sender)
        + sender->GetName() + ": "
        + text;

    LOG_INFO("server", "{}", plainLine);

    auto const& players = ObjectAccessor::GetPlayers();
    for (auto const& pair : players)
    {
        Player* recv = pair.second;
        if (!recv || !recv->IsInWorld())
            continue;
        if (!SameTeamOrCross(sender, recv))
            continue;

        ChatHandler(recv->GetSession()).SendSysMessage(coloredLine.c_str());
    }
}

/* -------- Delayed Login Announcement -------- */
static std::unordered_map<ObjectGuid, uint32> g_loginAnnounceRemainMs;

class WorldChat_AnnounceDelay : public WorldScript
{
public:
    WorldChat_AnnounceDelay() : WorldScript("WorldChat_AnnounceDelay") { }

    void OnUpdate(uint32 diff) override
    {
        if (g_loginAnnounceRemainMs.empty())
            return;

        for (auto it = g_loginAnnounceRemainMs.begin(); it != g_loginAnnounceRemainMs.end(); )
        {
            ObjectGuid guid = it->first;
            uint32& remain = it->second;

            if (remain <= diff)
            {
                if (Player* p = ObjectAccessor::FindPlayer(guid))
                {
                    ChatHandler(p->GetSession()).SendSysMessage("[Global Chat] Type \"/join Global\" to talk to all players on the server regardless of faction.");
                }
                it = g_loginAnnounceRemainMs.erase(it);
                continue;
            }

            remain -= diff;
            ++it;
        }
    }
};
/* ------------------------------------------- */

class WorldChat_Config : public WorldScript
{
public:
    WorldChat_Config() : WorldScript("WorldChat_Config") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        WC_Config.Enabled       = sConfigMgr->GetOption<bool>("World_Chat.Enable", true);
        WC_Config.ChannelName   = sConfigMgr->GetOption<std::string>("World_Chat.ChannelName", "Global");
        WC_Config.CrossFactions = sConfigMgr->GetOption<bool>("World_Chat.CrossFactions", true);
        WC_Config.Announce      = sConfigMgr->GetOption<bool>("World_Chat.Announce", true);
    }
};

class world_chat_commands : public CommandScript
{
public:
    world_chat_commands() : CommandScript("world_chat_commands") { }

    static bool HandleWorldChatCommand(ChatHandler* handler, Tail msgTail)
    {
        if (!WC_Config.Enabled)
            return true;

        Player* sender = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!sender)
            return true;

        std::string msg = std::string(msgTail);
        while (!msg.empty() && std::isspace(static_cast<unsigned char>(msg.front())))
            msg.erase(msg.begin());

        if (msg.empty())
        {
            handler->PSendSysMessage("Usage: .chat <message>");
            return true;
        }

        SendWorldMessage(sender, msg);
        return true;
    }

    ChatCommandTable GetCommands() const
    {
        static ChatCommandTable table
        {
            { "chat", HandleWorldChatCommand, SEC_PLAYER, Console::No }
        };
        return table;
    }
};

class WorldChat_Player : public PlayerScript
{
public:
    WorldChat_Player() : PlayerScript("WorldChat_Player") { }

    void OnLogin(Player* player)
    {
        if (!player)
            return;

        // Schedule a one-time announcement 10 seconds after login
        g_loginAnnounceRemainMs[player->GetGUID()] = 10000;
    }

    void OnLogout(Player* player)
    {
        if (!player)
            return;
        g_loginAnnounceRemainMs.erase(player->GetGUID());
    }

    void OnPlayerChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Channel* channel)
    {
        if (!WC_Config.Enabled || !player || !channel)
            return;

        if (lang == LANG_ADDON)
            return;

        if (WC_Config.ChannelName.empty())
            return;

        if (ToUpper(channel->GetName()) != ToUpper(WC_Config.ChannelName))
            return;

        SendWorldMessage(player, msg);
        msg.clear();
    }
};

void AddSC_WorldChatScripts()
{
    new WorldChat_Config();
    new world_chat_commands();
    new WorldChat_Player();
    new WorldChat_AnnounceDelay();
}
