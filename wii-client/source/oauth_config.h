/* Publisher configuration. Public IDs only; NEVER put client secrets here.
 * One Client ID is compiled in for every user. No per-user developer app
 * and no oauth.ini override. */
#ifndef OAUTH_CONFIG_H
#define OAUTH_CONFIG_H
#define OAUTH_TWITCH_CLIENT_ID "ug846nnimduejogw2rjnp6edg5eu6l" /* WiiStream, Public, redirect http://localhost */
#define OAUTH_KICK_CLIENT_ID ""   /* unused in TwitchWii */
#define OAUTH_KICK_RELAY_URL ""   /* unused in TwitchWii */
#define DEFAULT_TWITCH_CLIENT_ID OAUTH_TWITCH_CLIENT_ID
#define DEFAULT_KICK_RELAY_URL OAUTH_KICK_RELAY_URL
#endif
