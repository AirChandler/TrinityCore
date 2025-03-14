/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "LoginRESTService.h"
#include "Base64.h"
#include "Configuration/Config.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "IpNetwork.h"
#include "IteratorPair.h"
#include "ProtobufJSON.h"
#include "Resolver.h"
#include "Util.h"

namespace Battlenet
{
LoginRESTService& LoginRESTService::Instance()
{
    static LoginRESTService instance;
    return instance;
}

bool LoginRESTService::StartNetwork(Trinity::Asio::IoContext& ioContext, std::string const& bindIp, uint16 port, int32 threadCount)
{
    if (!HttpService::StartNetwork(ioContext, bindIp, port, threadCount))
        return false;

    using Trinity::Net::Http::RequestHandlerFlag;

    RegisterHandler(boost::beast::http::verb::get, "/bnetserver/login/", [this](std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
    {
        return HandleGetForm(std::move(session), context);
    });

    RegisterHandler(boost::beast::http::verb::get, "/bnetserver/gameAccounts/", [this](std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
    {
        return HandleGetGameAccounts(std::move(session), context);
    });

    RegisterHandler(boost::beast::http::verb::get, "/bnetserver/portal/", [this](std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
    {
        return HandleGetPortal(std::move(session), context);
    });

    RegisterHandler(boost::beast::http::verb::post, "/bnetserver/login/", [this](std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
    {
        return HandlePostLogin(std::move(session), context);
    }, RequestHandlerFlag::DoNotLogRequestContent);

    RegisterHandler(boost::beast::http::verb::post, "/bnetserver/refreshLoginTicket/", [this](std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
    {
        return HandlePostRefreshLoginTicket(std::move(session), context);
    });

    _bindIP = bindIp;
    _port = port;

    Trinity::Asio::Resolver resolver(ioContext);

    _hostnames[0] = sConfigMgr->GetStringDefault("LoginREST.ExternalAddress", "127.0.0.1");
    Optional<boost::asio::ip::tcp::endpoint> externalAddress = resolver.Resolve(boost::asio::ip::tcp::v4(), _hostnames[0], std::to_string(_port));
    if (!externalAddress)
    {
        TC_LOG_ERROR("server.http.login", "Could not resolve LoginREST.ExternalAddress {}", _hostnames[0]);
        return false;
    }

    _addresses[0] = externalAddress->address();

    _hostnames[1] = sConfigMgr->GetStringDefault("LoginREST.LocalAddress", "127.0.0.1");
    Optional<boost::asio::ip::tcp::endpoint> localAddress = resolver.Resolve(boost::asio::ip::tcp::v4(), _hostnames[1], std::to_string(_port));
    if (!localAddress)
    {
        TC_LOG_ERROR("server.http.login", "Could not resolve LoginREST.LocalAddress {}", _hostnames[1]);
        return false;
    }

    _addresses[1] = localAddress->address();

    // set up form inputs
    JSON::Login::FormInput* input;
    _formInputs.set_type(JSON::Login::LOGIN_FORM);
    input = _formInputs.add_inputs();
    input->set_input_id("account_name");
    input->set_type("text");
    input->set_label("E-mail");
    input->set_max_length(320);

    input = _formInputs.add_inputs();
    input->set_input_id("password");
    input->set_type("password");
    input->set_label("Password");
    input->set_max_length(16);

    input = _formInputs.add_inputs();
    input->set_input_id("log_in_submit");
    input->set_type("submit");
    input->set_label("Log In");

    _loginTicketDuration = sConfigMgr->GetIntDefault("LoginREST.TicketDuration", 3600);

    _acceptor->AsyncAcceptWithCallback<&LoginRESTService::OnSocketAccept>();
    return true;
}

std::string const& LoginRESTService::GetHostnameForClient(boost::asio::ip::address const& address) const
{
    if (auto addressIndex = Trinity::Net::SelectAddressForClient(address, _addresses))
        return _hostnames[*addressIndex];

    if (address.is_loopback())
        return _hostnames[1];

    return _hostnames[0];
}

std::string LoginRESTService::ExtractAuthorization(HttpRequest const& request)
{
    using namespace std::string_view_literals;

    std::string ticket;
    auto itr = request.find(boost::beast::http::field::authorization);
    if (itr == request.end())
        return ticket;

    std::string_view authorization = Trinity::Net::Http::ToStdStringView(itr->value());
    constexpr std::string_view BASIC_PREFIX = "Basic "sv;

    if (authorization.starts_with(BASIC_PREFIX))
        authorization.remove_prefix(BASIC_PREFIX.length());

    Optional<std::vector<uint8>> decoded = Trinity::Encoding::Base64::Decode(authorization);
    if (!decoded)
        return ticket;

    std::string_view decodedHeader(reinterpret_cast<char const*>(decoded->data()), decoded->size());

    if (std::size_t ticketEnd = decodedHeader.find(':'); ticketEnd != std::string_view::npos)
        decodedHeader.remove_suffix(decodedHeader.length() - ticketEnd);

    ticket = decodedHeader;
    return ticket;
}

LoginRESTService::RequestHandlerResult LoginRESTService::HandleGetForm(std::shared_ptr<LoginHttpSession> /*session*/, HttpRequestContext& context)
{
    context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
    context.response.body() = ::JSON::Serialize(_formInputs);
    return RequestHandlerResult::Handled;
}

LoginRESTService::RequestHandlerResult LoginRESTService::HandleGetGameAccounts(std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
{
    std::string ticket = ExtractAuthorization(context.request);
    if (ticket.empty())
        return HandleUnauthorized(std::move(session), context);

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_BNET_GAME_ACCOUNT_LIST);
    stmt->setString(0, ticket);
    session->QueueQuery(LoginDatabase.AsyncQuery(stmt)
        .WithPreparedCallback([session, context = std::move(context)](PreparedQueryResult result) mutable
    {
        JSON::Login::GameAccountList gameAccounts;
        if (result)
        {
            auto formatDisplayName = [](char const* name) -> std::string
            {
                if (char const* hashPos = strchr(name, '#'))
                    return std::string("WoW") + ++hashPos;
                else
                    return name;
            };

            time_t now = time(nullptr);
            do
            {
                Field* fields = result->Fetch();
                JSON::Login::GameAccountInfo* gameAccount = gameAccounts.add_game_accounts();
                gameAccount->set_display_name(formatDisplayName(fields[0].GetCString()));
                gameAccount->set_expansion(fields[1].GetUInt8());
                if (!fields[2].IsNull())
                {
                    uint32 banDate = fields[2].GetUInt32();
                    uint32 unbanDate = fields[3].GetUInt32();
                    gameAccount->set_is_suspended(unbanDate > now);
                    gameAccount->set_is_banned(banDate == unbanDate);
                    gameAccount->set_suspension_reason(fields[4].GetString());
                    gameAccount->set_suspension_expires(unbanDate);
                }
            } while (result->NextRow());
        }

        context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
        context.response.body() = ::JSON::Serialize(gameAccounts);
        session->SendResponse(context);
    }));

    return RequestHandlerResult::Async;
}

LoginRESTService::RequestHandlerResult LoginRESTService::HandleGetPortal(std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
{
    context.response.set(boost::beast::http::field::content_type, "text/plain");
    context.response.body() = Trinity::StringFormat("{}:{}", GetHostnameForClient(session->GetRemoteIpAddress()), sConfigMgr->GetIntDefault("BattlenetPort", 1119));
    return RequestHandlerResult::Handled;
}

LoginRESTService::RequestHandlerResult LoginRESTService::HandlePostLogin(std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
{
    JSON::Login::LoginForm loginForm;
    if (!::JSON::Deserialize(context.request.body(), &loginForm))
    {
        JSON::Login::LoginResult loginResult;
        loginResult.set_authentication_state(JSON::Login::LOGIN);
        loginResult.set_error_code("UNABLE_TO_DECODE");
        loginResult.set_error_message("There was an internal error while connecting to Battle.net. Please try again later.");

        context.response.result(boost::beast::http::status::bad_request);
        context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
        context.response.body() = ::JSON::Serialize(loginResult);
        session->SendResponse(context);

        return RequestHandlerResult::Handled;
    }

    std::string login;
    std::string password;

    for (int32 i = 0; i < loginForm.inputs_size(); ++i)
    {
        if (loginForm.inputs(i).input_id() == "account_name")
            login = loginForm.inputs(i).value();
        else if (loginForm.inputs(i).input_id() == "password")
            password = loginForm.inputs(i).value();
    }

    Utf8ToUpperOnlyLatin(login);
    Utf8ToUpperOnlyLatin(password);

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_BNET_AUTHENTICATION);
    stmt->setString(0, login);

    std::string sentPasswordHash = CalculateShaPassHash(login, password);

    session->QueueQuery(LoginDatabase.AsyncQuery(stmt)
        .WithChainingPreparedCallback([this, session, context = std::move(context), login = std::move(login), sentPasswordHash = std::move(sentPasswordHash)](QueryCallback& callback, PreparedQueryResult result) mutable
    {
        if (!result)
        {
            JSON::Login::LoginResult loginResult;
            loginResult.set_authentication_state(JSON::Login::DONE);
            context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
            context.response.body() = ::JSON::Serialize(loginResult);
            session->SendResponse(context);
            return;
        }

        Field* fields = result->Fetch();
        uint32 accountId = fields[0].GetUInt32();
        std::string pass_hash = fields[1].GetString();
        uint32 failedLogins = fields[2].GetUInt32();
        std::string loginTicket = fields[3].GetString();
        uint32 loginTicketExpiry = fields[4].GetUInt32();
        bool isBanned = fields[5].GetUInt64() != 0;

        if (sentPasswordHash != pass_hash)
        {
            if (!isBanned)
            {
                std::string ip_address = session->GetRemoteIpAddress().to_string();
                uint32 maxWrongPassword = uint32(sConfigMgr->GetIntDefault("WrongPass.MaxCount", 0));

                if (sConfigMgr->GetBoolDefault("WrongPass.Logging", false))
                    TC_LOG_DEBUG("server.http.login", "[{}, Account {}, Id {}] Attempted to connect with wrong password!", ip_address, login, accountId);

                if (maxWrongPassword)
                {
                    LoginDatabaseTransaction trans = LoginDatabase.BeginTransaction();
                    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_BNET_FAILED_LOGINS);
                    stmt->setUInt32(0, accountId);
                    trans->Append(stmt);

                    ++failedLogins;

                    TC_LOG_DEBUG("server.http.login", "MaxWrongPass : {}, failed_login : {}", maxWrongPassword, accountId);

                    if (failedLogins >= maxWrongPassword)
                    {
                        BanMode banType = BanMode(sConfigMgr->GetIntDefault("WrongPass.BanType", uint16(BanMode::BAN_IP)));
                        int32 banTime = sConfigMgr->GetIntDefault("WrongPass.BanTime", 600);

                        if (banType == BanMode::BAN_ACCOUNT)
                        {
                            stmt = LoginDatabase.GetPreparedStatement(LOGIN_INS_BNET_ACCOUNT_AUTO_BANNED);
                            stmt->setUInt32(0, accountId);
                        }
                        else
                        {
                            stmt = LoginDatabase.GetPreparedStatement(LOGIN_INS_IP_AUTO_BANNED);
                            stmt->setString(0, ip_address);
                        }

                        stmt->setUInt32(1, banTime);
                        trans->Append(stmt);

                        stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_BNET_RESET_FAILED_LOGINS);
                        stmt->setUInt32(0, accountId);
                        trans->Append(stmt);
                    }

                    LoginDatabase.CommitTransaction(trans);
                }
            }

            JSON::Login::LoginResult loginResult;
            loginResult.set_authentication_state(JSON::Login::DONE);

            context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
            context.response.body() = ::JSON::Serialize(loginResult);
            session->SendResponse(context);
            return;
        }

        if (loginTicket.empty() || loginTicketExpiry < time(nullptr))
            loginTicket = "TC-" + ByteArrayToHexStr(Trinity::Crypto::GetRandomBytes<20>());

        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_BNET_AUTHENTICATION);
        stmt->setString(0, loginTicket);
        stmt->setUInt32(1, time(nullptr) + _loginTicketDuration);
        stmt->setUInt32(2, accountId);
        callback.WithPreparedCallback([session, context = std::move(context), loginTicket = std::move(loginTicket)](PreparedQueryResult) mutable
        {
            JSON::Login::LoginResult loginResult;
            loginResult.set_authentication_state(JSON::Login::DONE);
            loginResult.set_login_ticket(loginTicket);

            context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
            context.response.body() = ::JSON::Serialize(loginResult);
            session->SendResponse(context);
        }).SetNextQuery(LoginDatabase.AsyncQuery(stmt));
    }));

    return RequestHandlerResult::Async;
}

LoginRESTService::RequestHandlerResult LoginRESTService::HandlePostRefreshLoginTicket(std::shared_ptr<LoginHttpSession> session, HttpRequestContext& context)
{
    std::string ticket = ExtractAuthorization(context.request);
    if (ticket.empty())
        return HandleUnauthorized(std::move(session), context);

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_BNET_EXISTING_AUTHENTICATION);
    stmt->setString(0, ticket);
    session->QueueQuery(LoginDatabase.AsyncQuery(stmt)
        .WithPreparedCallback([this, session, context = std::move(context), ticket = std::move(ticket)](PreparedQueryResult result) mutable
    {
        JSON::Login::LoginRefreshResult loginRefreshResult;
        if (result)
        {
            uint32 loginTicketExpiry = (*result)[0].GetUInt32();
            time_t now = time(nullptr);
            if (loginTicketExpiry > now)
            {
                loginRefreshResult.set_login_ticket_expiry(now + _loginTicketDuration);

                LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_BNET_EXISTING_AUTHENTICATION);
                stmt->setUInt32(0, uint32(now + _loginTicketDuration));
                stmt->setString(1, ticket);
                LoginDatabase.Execute(stmt);
            }
            else
                loginRefreshResult.set_is_expired(true);
        }
        else
            loginRefreshResult.set_is_expired(true);

        context.response.set(boost::beast::http::field::content_type, "application/json;charset=utf-8");
        context.response.body() = ::JSON::Serialize(loginRefreshResult);
        session->SendResponse(context);
    }));

    return RequestHandlerResult::Async;
}

std::string LoginRESTService::CalculateShaPassHash(std::string const& name, std::string const& password)
{
    Trinity::Crypto::SHA256 email;
    email.UpdateData(name);
    email.Finalize();

    Trinity::Crypto::SHA256 sha;
    sha.UpdateData(ByteArrayToHexStr(email.GetDigest()));
    sha.UpdateData(":");
    sha.UpdateData(password);
    sha.Finalize();

    return ByteArrayToHexStr(sha.GetDigest(), true);
}

void LoginRESTService::OnSocketAccept(boost::asio::ip::tcp::socket&& sock, uint32 threadIndex)
{
    sLoginService.OnSocketOpen(std::move(sock), threadIndex);
}
}
