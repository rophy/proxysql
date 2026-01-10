/**
 * @file auth_static.cpp
 * @brief Static authentication plugin for ProxySQL
 *
 * This plugin validates credentials against a static password configured
 * in the user's attributes JSON field. Useful for testing and simple
 * authentication scenarios.
 *
 * User configuration example:
 *   INSERT INTO mysql_users (username, password, attributes, default_hostgroup)
 *   VALUES ('testuser', '', '{"auth_plugin": "static", "static_password": "secret123"}', 1);
 *
 * The plugin compares the client-provided password against the "static_password"
 * value in the attributes JSON.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../include/proxysql_auth_plugin.hpp"
#include "../../../deps/json/json.hpp"

using json = nlohmann::json;

class Static_Authentication : public MySQL_LDAP_Authentication {
public:
    Static_Authentication() {
        fprintf(stderr, "Static Auth Plugin initialized\n");
    }

    ~Static_Authentication() override {
        fprintf(stderr, "Static Auth Plugin destroyed\n");
    }

    void print_version() override {
        fprintf(stderr, "ProxySQL Static Auth Plugin v1.0\n");
    }

    /**
     * @brief Authenticate user against static password in attributes
     *
     * @param username      Frontend username
     * @param pass          Clear-text password from client
     * @param usertype      USERNAME_FRONTEND or USERNAME_BACKEND
     * @param use_ssl       OUT: require SSL for backend
     * @param default_hostgroup OUT: hostgroup for this user
     * @param default_schema OUT: default schema
     * @param schema_locked OUT: schema locked flag
     * @param transaction_persistent OUT: transaction persistent flag
     * @param fast_forward  OUT: fast forward flag
     * @param max_connections OUT: max connections
     * @param sha1_pass     OUT: SHA1 password hash
     * @param attributes    IN/OUT: user attributes JSON
     * @param backend_username OUT: backend username (if different from frontend)
     *
     * @return Password string on success (caller must free), NULL on failure
     */
    char* lookup(
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
    ) override {
        if (!username || !pass || !attributes || !*attributes) {
            fprintf(stderr, "Static Auth: missing required parameters\n");
            return nullptr;
        }

        try {
            json attrs = json::parse(*attributes);

            // Get static_password from attributes
            auto it = attrs.find("static_password");
            if (it == attrs.end()) {
                fprintf(stderr, "Static Auth: 'static_password' not found in attributes for user '%s'\n", username);
                return nullptr;
            }

            std::string expected_password = it->get<std::string>();

            // Compare passwords
            if (expected_password != pass) {
                fprintf(stderr, "Static Auth: password mismatch for user '%s'\n", username);
                return nullptr;
            }

            fprintf(stderr, "Static Auth: authentication successful for user '%s'\n", username);

            // Check for backend_username mapping
            auto backend_it = attrs.find("backend_username");
            if (backend_it != attrs.end() && backend_username) {
                std::string bu = backend_it->get<std::string>();
                *backend_username = strdup(bu.c_str());
                fprintf(stderr, "Static Auth: mapping frontend user '%s' to backend user '%s'\n",
                    username, *backend_username);
            }

            // Return the password (this is what ProxySQL will use for backend auth)
            // For static auth, we just return the same password
            return strdup(pass);

        } catch (json::exception& e) {
            fprintf(stderr, "Static Auth: JSON parse error for user '%s': %s\n", username, e.what());
            return nullptr;
        }
    }

    // Connection tracking (optional - using defaults)
    int increase_frontend_user_connections(char* username, int* max_connections) override {
        return 0;
    }

    void decrease_frontend_user_connections(char* username) override {
    }
};

// Required exports for ProxySQL plugin system

extern "C" const char* auth_plugin_name() {
    return "static";
}

extern "C" MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_func() {
    return new Static_Authentication();
}
