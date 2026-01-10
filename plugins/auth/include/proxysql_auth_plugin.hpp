/**
 * @file proxysql_auth_plugin.hpp
 * @brief Minimal header for ProxySQL authentication plugins
 *
 * This header provides the interface that auth plugins must implement,
 * without requiring all of ProxySQL's internal headers.
 */

#ifndef PROXYSQL_AUTH_PLUGIN_H
#define PROXYSQL_AUTH_PLUGIN_H

#include <cstdint>
#include <cstdlib>

// Credential type enum (matches ProxySQL's definition)
enum cred_username_type {
    USERNAME_FRONTEND,
    USERNAME_BACKEND
};

/**
 * @brief Base class for ProxySQL authentication plugins
 *
 * Plugins must inherit from this class and implement the lookup() method.
 * The class is designed to be ABI-compatible with MySQL_LDAP_Authentication.
 */
class MySQL_LDAP_Authentication {
public:
    virtual ~MySQL_LDAP_Authentication() {}

    /**
     * @brief Authenticate a user
     *
     * @param username      Frontend username
     * @param pass          Clear-text password/token from client
     * @param usertype      USERNAME_FRONTEND or USERNAME_BACKEND
     * @param use_ssl       OUT: require SSL for backend
     * @param default_hostgroup OUT: hostgroup for this user
     * @param default_schema OUT: default schema (caller frees if set)
     * @param schema_locked OUT: schema locked flag
     * @param transaction_persistent OUT: transaction persistent flag
     * @param fast_forward  OUT: fast forward flag
     * @param max_connections OUT: max connections
     * @param sha1_pass     OUT: SHA1 password hash (20 bytes, caller frees)
     * @param attributes    IN/OUT: user attributes JSON (caller frees if changed)
     * @param backend_username OUT: backend username if different (caller frees)
     *
     * @return Password string on success (caller must free), NULL on failure
     */
    virtual char* lookup(
        char* username,
        char* pass,
        enum cred_username_type usertype,
        bool* use_ssl,
        int* default_hostgroup,
        char** default_schema,
        bool* schema_locked,
        bool* transaction_persistent,
        bool* fast_forward,
        int* max_connections,
        void** sha1_pass,
        char** attributes,
        char** backend_username
    ) { return NULL; }

    // Optional: connection tracking
    virtual int increase_frontend_user_connections(char* username, int* max_connections = NULL) { return 0; }
    virtual void decrease_frontend_user_connections(char* username) {}

    // Optional: plugin variables
    virtual char** get_variables_list() { return NULL; }
    virtual bool has_variable(const char* name) { return false; }
    virtual char* get_variable(char* name) { return NULL; }
    virtual bool set_variable(char* name, char* value) { return false; }

    // Optional: version info
    virtual void print_version() {}
};

// Factory function type that plugins must export
typedef MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_t();

/**
 * Plugins must export these two C functions:
 *
 * extern "C" const char* auth_plugin_name();
 * extern "C" MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_func();
 */

#endif /* PROXYSQL_AUTH_PLUGIN_H */
