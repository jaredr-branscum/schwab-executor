#include "schwab/token.hpp"
#include "schwab/time.hpp"

namespace schwab {

void OAuthToken::precompute_auth_header() {
    if (!access_token.empty()) {
        cached_auth_header = "Bearer " + access_token;
    } else {
        cached_auth_header.clear();
    }
}

std::string_view OAuthToken::auth_header() const noexcept {
    return cached_auth_header;
}

bool OAuthToken::is_access_token_expired(double buffer_seconds, double now_override) const noexcept {
    if (access_token.empty() || expires_at <= 0.0) {
        return true;
    }
    double now = (now_override > 0.0) ? now_override : epoch_seconds_now();
    return (now + buffer_seconds >= expires_at);
}

bool OAuthToken::is_refresh_token_expired(double buffer_seconds, double now_override) const noexcept {
    if (refresh_token.empty() || refresh_token_expires_at <= 0.0) {
        return true;
    }
    double now = (now_override > 0.0) ? now_override : epoch_seconds_now();
    return (now + buffer_seconds >= refresh_token_expires_at);
}

} // namespace schwab
