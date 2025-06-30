#include <config.h>

#if USE_JWT_CPP && USE_SSL
#include <Client/JwtProvider.h>
#include <Common/Exception.h>

#include <Client/CloudJwtProvider.h>
#include <Common/StringUtils.h>

#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/SSLManager.h>

#include <jwt-cpp/jwt.h>

#include <thread>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace DB
{

namespace ErrorCodes
{
    extern const int SUPPORT_IS_DISABLED;
}

JwtProvider::JwtProvider(
    std::string auth_url,
    std::string client_id,
    std::ostream & out,
    std::ostream & err)
    : auth_url_str(std::move(auth_url)),
      client_id_str(std::move(client_id)),
      output_stream(out),
      error_stream(err)
{
}

std::string JwtProvider::getJWT()
{
    Poco::Timestamp now;
    Poco::Timestamp expiration_buffer = 15 * Poco::Timespan::SECONDS;

    if (!idp_access_token.empty() && now < idp_access_token_expires_at - expiration_buffer)
        return idp_access_token;

    if (!idp_refresh_token.empty())
    {
        if (refreshIdPAccessToken())
            return idp_access_token;
    }

    if (initialLogin())
        return idp_access_token;

    error_stream << "Failed to obtain a valid JWT from the external provider." << std::endl;
    return "";
}

bool JwtProvider::initialLogin()
{
    Poco::URI auth_uri_check(auth_url_str);
    std::string base_auth_url = auth_uri_check.getScheme() + "://" + auth_uri_check.getAuthority();
    std::string device_code_url_str = base_auth_url + "/oauth/device/code";
    std::string token_url_str = base_auth_url + "/oauth/token";

    if (client_id_str.empty())
    {
        error_stream << "Error: --auth-client-id is required for --login." << std::endl;
        return false;
    }
    std::string scope = "openid profile email offline_access";
    std::string audience = "control-plane-web";

    std::string device_code;
    int interval_seconds = 5;
    Poco::Timestamp::TimeVal expires_at_ts = 0;

    try
    {
        Poco::URI device_code_uri(device_code_url_str);
        auto session = createHTTPSession(device_code_uri);
        Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, device_code_uri.getPathAndQuery(), Poco::Net::HTTPMessage::HTTP_1_1);
        request.setContentType("application/x-www-form-urlencoded");

        std::string encoded_scope;
        Poco::URI::encode(scope, "", encoded_scope);
        std::string encoded_audience;
        Poco::URI::encode(audience, "", encoded_audience);
        std::string request_body = "client_id=" + client_id_str + "&scope=" + encoded_scope + "&audience=" + encoded_audience;

        request.setContentLength(request_body.length());
        session->sendRequest(request) << request_body;

        Poco::Net::HTTPResponse response;
        std::istream & rs = session->receiveResponse(response);
        if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
        {
            std::string error_body;
            Poco::StreamCopier::copyToString(rs, error_body);
            error_stream << "Error requesting device code: " << response.getStatus() << " " << response.getReason() << "\nResponse: " << error_body << std::endl;
            return false;
        }

        Poco::JSON::Object::Ptr object = Poco::JSON::Parser().parse(rs).extract<Poco::JSON::Object::Ptr>();
        device_code = object->getValue<std::string>("device_code");
        std::string user_code = object->getValue<std::string>("user_code");
        std::string verification_uri_complete = object->getValue<std::string>("verification_uri_complete");
        interval_seconds = object->getValue<int>("interval");
        expires_at_ts = Poco::Timestamp().epochTime() + object->getValue<int>("expires_in");

        output_stream << "\nOpening your browser for login. If it doesn't open, visit:"
                      << "\n\n        " << verification_uri_complete << "\n\n"
                      << "Then enter the code: \033[1m" << user_code << "\033[0m\n" << std::endl;

        openURLInBrowser(verification_uri_complete);
    }
    catch (const Poco::Exception & e)
    {
        error_stream << "Exception during device code request: " << e.displayText() << std::endl;
        return false;
    }

    Poco::Timestamp now;
    while (now.epochTime() < expires_at_ts)
    {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        now.update();
        try
        {
            Poco::URI token_uri(token_url_str);
            auto session = createHTTPSession(token_uri);
            Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, token_uri.getPathAndQuery(), Poco::Net::HTTPMessage::HTTP_1_1);
            request.setContentType("application/x-www-form-urlencoded");
            std::string request_body = "grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=" + device_code + "&client_id=" + client_id_str;
            request.setContentLength(request_body.length());
            session->sendRequest(request) << request_body;

            Poco::Net::HTTPResponse response;
            std::istream & rs = session->receiveResponse(response);
            std::string response_body;
            Poco::StreamCopier::copyToString(rs, response_body);
            Poco::JSON::Object::Ptr object = Poco::JSON::Parser().parse(response_body).extract<Poco::JSON::Object::Ptr>();

            if (response.getStatus() == Poco::Net::HTTPResponse::HTTP_OK)
            {
                idp_access_token = object->getValue<std::string>("access_token");
                idp_access_token_expires_at = Poco::Timestamp::fromEpochTime(jwt::decode(idp_access_token).get_payload_claim("exp").as_integer());
                if (object->has("refresh_token"))
                    idp_refresh_token = object->getValue<std::string>("refresh_token");
                return true;
            }

            std::string error = object->optValue<std::string>("error", "unknown_error");
            if (error != "authorization_pending" && error != "slow_down")
            {
                error_stream << "IdP login failed: " << object->optValue<std::string>("error_description", error) << std::endl;
                return false;
            }
        }
        catch (const Poco::Exception & e)
        {
            error_stream << "Error waiting for authorization: " << e.displayText() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    error_stream << "Device login flow timed out." << std::endl;
    return false;
}

bool JwtProvider::refreshIdPAccessToken()
{
    Poco::URI auth_uri_check(auth_url_str);
    std::string token_url_str = auth_uri_check.getScheme() + "://" + auth_uri_check.getAuthority() + "/oauth/token";

    try
    {
        Poco::URI token_uri(token_url_str);
        auto session = createHTTPSession(token_uri);
        Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, token_uri.getPathAndQuery(), Poco::Net::HTTPMessage::HTTP_1_1);
        request.setContentType("application/x-www-form-urlencoded");

        std::string request_body = "grant_type=refresh_token&client_id=" + client_id_str + "&refresh_token=" + idp_refresh_token;
        request.setContentLength(request_body.length());
        session->sendRequest(request) << request_body;

        Poco::Net::HTTPResponse response;
        std::istream & rs = session->receiveResponse(response);
        if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
        {
            std::string error_body;
            Poco::StreamCopier::copyToString(rs, error_body);
            error_stream << "Error refreshing token: " << response.getStatus() << " " << response.getReason() << "\nResponse: " << error_body << std::endl;
            idp_refresh_token.clear();
            return false;
        }

        Poco::JSON::Object::Ptr object = Poco::JSON::Parser().parse(rs).extract<Poco::JSON::Object::Ptr>();
        idp_access_token = object->getValue<std::string>("access_token");
        idp_access_token_expires_at = Poco::Timestamp::fromEpochTime(jwt::decode(idp_access_token).get_payload_claim("exp").as_integer());
        if (object->has("refresh_token"))
             idp_refresh_token = object->getValue<std::string>("refresh_token");

        return true;
    }
    catch (const Poco::Exception & e)
    {
        error_stream << "Exception during token refresh: " << e.displayText() << std::endl;
        return false;
    }
}

std::unique_ptr<Poco::Net::HTTPSClientSession> JwtProvider::createHTTPSession(const Poco::URI & uri)
{
    if (uri.getScheme() == "https")
    {
        Poco::Net::Context::Ptr context = Poco::Net::SSLManager::instance().defaultClientContext();
        return std::make_unique<Poco::Net::HTTPSClientSession>(uri.getHost(), uri.getPort(), context);
    }
    throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Built without SSL, ClickHouse cannot use JWT authentication without SSL support.");
}

bool JwtProvider::openURLInBrowser(const std::string & url)
{
    if (url.empty())
        return true;

    std::string command;
#if defined(OS_DARWIN)
    command = "open";
#elif defined(OS_LINUX)
    command = "xdg-open";
#elif defined(_WIN32)
    command = "start";
#endif

    if (command.empty())
        return false;

    return std::system((command + " " + url).c_str()) == 0;
}

Poco::Timestamp JwtProvider::getJwtExpiry(const std::string & token)
{
    if (token.empty())
        return 0;

    try
    {
        auto decoded_token = jwt::decode(token);
        return Poco::Timestamp::fromEpochTime(decoded_token.get_payload_claim("exp").as_integer());
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

bool isCloudEndpoint(const std::string & host)
{
    return endsWith(host, ".clickhouse.cloud") || endsWith(host, ".clickhouse-staging.com") || endsWith(host, ".clickhouse-dev.com");
}

std::unique_ptr<JwtProvider> createJwtProvider(
    const std::string & auth_url,
    const std::string & client_id,
    const std::string & host,
    std::ostream & out,
    std::ostream & err)
{
    if (isCloudEndpoint(host))
    {
        return std::make_unique<CloudJwtProvider>(auth_url, client_id, host, out, err);
    }
    else
    {
        return std::make_unique<JwtProvider>(auth_url, client_id, out, err);
    }
}

}

#endif
