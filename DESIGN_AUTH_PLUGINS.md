# Per-User Authentication Plugins Design

## Overview

This document describes a design to enable per-user authentication plugins in ProxySQL via the existing `mysql_users.attributes` JSON field. This allows different users to authenticate using different methods (e.g., Kubernetes ServiceAccount tokens, OAuth, custom auth) while leveraging ProxySQL's existing plugin architecture.

## Motivation

ProxySQL already supports:
1. **Client/backend auth separation**: Clients can authenticate to ProxySQL differently than ProxySQL authenticates to backends
2. **LDAP authentication**: Via an enterprise plugin loaded at startup
3. **User attributes**: JSON field in `mysql_users` for per-user configuration

However, the current LDAP plugin is triggered when a user is **not found** in `mysql_users`. This design extends the architecture to support per-user auth plugin selection for users that **do exist** in `mysql_users`.

### Use Case: Kubernetes ServiceAccount Authentication

Applications running in Kubernetes can authenticate to ProxySQL using their ServiceAccount token instead of static passwords:

```
Client (SA token) → ProxySQL (validates token) → MariaDB (regular credentials)
```

Benefits:
- No static passwords in application configs
- Tokens are short-lived and auto-rotated
- Identity tied to Kubernetes workload identity

## User Configuration

### Enabling Auth Plugin for a User

```sql
-- User with K8s ServiceAccount authentication
INSERT INTO mysql_users (username, password, attributes, default_hostgroup)
VALUES (
    'local/proxysql/myapp',
    '',
    '{"auth_plugin": "k8s"}',
    1
);

-- Plugin-specific configuration can be included in attributes
INSERT INTO mysql_users (username, password, attributes, default_hostgroup)
VALUES (
    'local/proxysql/myapp',
    '',
    '{
        "auth_plugin": "k8s",
        "k8s_validator": "http://kube-federated-auth:8080/validate",
        "k8s_audience": "mariadb"
    }',
    1
);

LOAD MYSQL USERS TO RUNTIME;
```

### Standard Users (Unchanged)

```sql
-- Users without auth_plugin attribute use standard password auth
INSERT INTO mysql_users (username, password, default_hostgroup)
VALUES ('regular_user', 'password123', 1);
```

## ProxySQL Configuration

### Config File (`proxysql.cfg`)

```
# Existing LDAP plugin (unchanged, backward compatible)
ldap_auth_plugin="/path/to/ldap.so"

# New: additional auth plugins (comma-separated paths)
auth_plugins="/usr/lib/proxysql/auth_k8s.so,/usr/lib/proxysql/auth_oauth.so"
```

### Plugin Naming Convention

The plugin name is determined by the `auth_plugin_name()` export from the `.so` file, not the filename. However, a recommended convention is:

```
auth_<name>.so  →  exports name "<name>"

Examples:
  auth_k8s.so   →  "k8s"
  auth_oauth.so →  "oauth"
```

## Implementation Details

### 1. Configuration Variables

**File: `include/proxysql_glovars.hpp`**

```cpp
struct global_variables {
    // ... existing fields ...
    char * ldap_auth_plugin;   // existing
    char * auth_plugins;       // NEW: comma-separated plugin paths
};
```

**File: `lib/ProxySQL_GloVars.cpp`**

```cpp
// In destructor/cleanup
if (auth_plugins) {
    free(auth_plugins);
    auth_plugins = NULL;
}

// In initialization
auth_plugins = NULL;
```

### 2. Config Parsing

**File: `src/main.cpp`**

```cpp
// Add after ldap_auth_plugin parsing (~line 786)
if (root.exists("auth_plugins")==true) {
    string auth_plugins;
    rc=root.lookupValue("auth_plugins", auth_plugins);
    GloVars.auth_plugins=strdup(auth_plugins.c_str());
}
```

### 3. Plugin Registry

**New global variable (declare in appropriate header, define in `main.cpp`):**

```cpp
#include <map>
#include <string>

// Registry mapping plugin name to instance
std::map<std::string, MySQL_LDAP_Authentication*> GloAuthPlugins;
```

### 4. Plugin Loading

**File: `src/main.cpp` - Extend `LoadPlugins()`**

```cpp
static void LoadPlugins() {
    // ... existing LDAP loading code unchanged ...

    // NEW: Load additional auth plugins
    if (GloVars.auth_plugins) {
        std::string plugins_str(GloVars.auth_plugins);
        std::stringstream ss(plugins_str);
        std::string plugin_path;

        while (std::getline(ss, plugin_path, ',')) {
            // Trim whitespace
            plugin_path.erase(0, plugin_path.find_first_not_of(" \t"));
            plugin_path.erase(plugin_path.find_last_not_of(" \t") + 1);

            if (plugin_path.empty()) continue;

            dlerror(); // Clear errors
            void* handle = dlopen(plugin_path.c_str(), RTLD_NOW);
            if (!handle) {
                proxy_error("Cannot load auth plugin '%s': %s\n",
                    plugin_path.c_str(), dlerror());
                continue;
            }

            // Get plugin name
            dlerror();
            auto get_name = (const char*(*)()) dlsym(handle, "auth_plugin_name");
            char* dlsym_error = dlerror();
            if (dlsym_error || !get_name) {
                proxy_error("Auth plugin '%s' missing auth_plugin_name export\n",
                    plugin_path.c_str());
                dlclose(handle);
                continue;
            }
            const char* name = get_name();

            // Check for duplicate
            if (GloAuthPlugins.find(name) != GloAuthPlugins.end()) {
                proxy_error("Auth plugin '%s' already loaded, skipping '%s'\n",
                    name, plugin_path.c_str());
                dlclose(handle);
                continue;
            }

            // Create instance (reuse LDAP interface)
            dlerror();
            auto create_func = (create_MySQL_LDAP_Authentication_t*)
                dlsym(handle, "create_MySQL_LDAP_Authentication_func");
            dlsym_error = dlerror();
            if (dlsym_error || !create_func) {
                proxy_error("Auth plugin '%s' missing create_MySQL_LDAP_Authentication_func\n",
                    plugin_path.c_str());
                dlclose(handle);
                continue;
            }

            MySQL_LDAP_Authentication* plugin = create_func();
            if (!plugin) {
                proxy_error("Failed to create auth plugin '%s'\n", name);
                dlclose(handle);
                continue;
            }

            GloAuthPlugins[name] = plugin;
            proxy_info("Loaded auth plugin: %s from %s\n", name, plugin_path.c_str());
            plugin->print_version();
        }
    }
}
```

### 5. Auth Flow Modification

**File: `lib/MySQL_Protocol.cpp`**

Add check in the authentication flow after user lookup. The check should happen early, before password verification, to trigger AUTH_SWITCH if needed.

```cpp
// Helper function to check for auth_plugin attribute
static MySQL_LDAP_Authentication* get_user_auth_plugin(const char* attributes) {
    if (!attributes || strlen(attributes) == 0) {
        return nullptr;
    }

    try {
        nlohmann::json attrs = nlohmann::json::parse(attributes);
        auto it = attrs.find("auth_plugin");
        if (it == attrs.end()) {
            return nullptr;
        }

        std::string plugin_name = it->get<std::string>();
        auto plugin_it = GloAuthPlugins.find(plugin_name);
        if (plugin_it == GloAuthPlugins.end()) {
            proxy_error("Auth plugin '%s' specified but not loaded\n", plugin_name.c_str());
            return nullptr;  // Will cause auth failure
        }

        return plugin_it->second;
    } catch (nlohmann::json::exception& e) {
        return nullptr;
    }
}
```

Modification in `process_pkt_handshake_response()` or relevant PPHR function:

```cpp
// After user lookup, before password verification
account_details = GloMyAuth->lookup((char*)vars1.user, USERNAME_FRONTEND, dup_details);

// Check for per-user auth plugin
MySQL_LDAP_Authentication* user_auth_plugin = get_user_auth_plugin(account_details.attributes);

if (user_auth_plugin) {
    // User has auth_plugin specified
    if ((*myds)->switching_auth_stage == 0) {
        // Need to switch to clear password to get the token
        (*myds)->switching_auth_type = AUTH_MYSQL_CLEAR_PASSWORD;
        (*myds)->switching_auth_stage = 1;
        (*myds)->auth_in_progress = 1;
        generate_pkt_auth_switch_request(true, NULL, NULL);
        (*myds)->myconn->userinfo->set((char *)vars1.user, NULL, vars1.db, NULL);
        ret = false;
        goto __exit_process_pkt_handshake_response;
    }

    // After AUTH_SWITCH, we have clear-text password (token)
    // Validate via plugin (similar to LDAP flow)
    char *backend_username = NULL;
    char *validated_password = user_auth_plugin->lookup(
        (char*)vars1.user,
        (char*)vars1.pass,  // Clear-text token
        USERNAME_FRONTEND,
        &account_details.use_ssl,
        &account_details.default_hostgroup,
        &account_details.default_schema,
        &account_details.schema_locked,
        &account_details.transaction_persistent,
        &account_details.fast_forward,
        &account_details.max_connections,
        &account_details.sha1_pass,
        &account_details.attributes,
        &backend_username
    );

    if (validated_password) {
        // Auth successful
        vars1.password = validated_password;
        ret = true;
        // Handle backend_username mapping if provided
        if (backend_username) {
            // Use backend_username for backend connections
            // Similar to LDAP backend user mapping
        }
    } else {
        // Auth failed
        ret = false;
    }

    goto __exit_do_auth;
}

// ... continue with normal password verification ...
```

## Plugin Interface

Plugins reuse the existing `MySQL_LDAP_Authentication` interface to minimize changes.

### Required Exports

Each plugin `.so` must export:

```cpp
// 1. Plugin name (NEW - required for registry)
extern "C" const char* auth_plugin_name() {
    return "k8s";  // Unique name for this plugin
}

// 2. Factory function (EXISTING - same as LDAP)
extern "C" MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_func() {
    return new My_Auth_Plugin_Implementation();
}
```

### Interface Definition

From `include/MySQL_LDAP_Authentication.hpp`:

```cpp
class MySQL_LDAP_Authentication {
public:
    virtual ~MySQL_LDAP_Authentication() {};

    // Main authentication method
    // Returns: password for backend auth on success, NULL on failure
    virtual char * lookup(
        char *username,                    // Frontend username
        char *pass,                        // Clear-text password/token
        enum cred_username_type usertype,  // USERNAME_FRONTEND or USERNAME_BACKEND
        bool *use_ssl,                     // OUT: require SSL for backend
        int *default_hostgroup,            // OUT: hostgroup for this user
        char **default_schema,             // OUT: default schema
        bool *schema_locked,               // OUT: schema locked flag
        bool *transaction_persistent,      // OUT: transaction persistent flag
        bool *fast_forward,                // OUT: fast forward flag
        int *max_connections,              // OUT: max connections
        void **sha1_pass,                  // OUT: SHA1 password hash
        char **attributes,                 // OUT: modified attributes
        char **backend_username            // OUT: backend username (if different)
    ) { return NULL; }

    // Connection tracking (optional)
    virtual int increase_frontend_user_connections(char *username, int *max_connections = NULL) { return 0; }
    virtual void decrease_frontend_user_connections(char *username) {}

    // Plugin variables (optional)
    virtual char **get_variables_list() { return NULL; }
    virtual bool has_variable(const char *name) { return false; }
    virtual char * get_variable(char *name) { return NULL; }
    virtual bool set_variable(char *name, char *value) { return false; }

    // Version info
    virtual void print_version() {}
};
```

## Example: K8s Auth Plugin

### Plugin Implementation Skeleton

```cpp
// auth_k8s.cpp

#include "MySQL_LDAP_Authentication.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>

class K8s_Authentication : public MySQL_LDAP_Authentication {
private:
    std::string default_validator_url = "http://localhost:8080/validate";

public:
    void print_version() override {
        fprintf(stderr, "K8s Auth Plugin v1.0\n");
    }

    char* lookup(
        char *username,
        char *pass,  // This is the K8s SA token
        enum cred_username_type usertype,
        bool *use_ssl,
        int *default_hostgroup,
        char **default_schema,
        bool *schema_locked,
        bool *transaction_persistent,
        bool *fast_forward,
        int *max_connections,
        void **sha1_pass,
        char **attributes,
        char **backend_username
    ) override {
        // 1. Parse expected identity from username
        //    Format: "local/namespace/serviceaccount"

        // 2. Call validator service to validate token
        //    POST to validator_url with token
        //    Validator returns: { "valid": true, "subject": "system:serviceaccount:ns:sa" }

        // 3. Compare validated identity with expected identity

        // 4. On success, return backend credentials
        //    - Set *backend_username if mapping to different user
        //    - Return password hash for backend auth

        // 5. On failure, return NULL

        return nullptr;  // Implement actual validation
    }
};

// Required exports
extern "C" const char* auth_plugin_name() {
    return "k8s";
}

extern "C" MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_func() {
    return new K8s_Authentication();
}
```

### Build Plugin

```bash
g++ -shared -fPIC -o auth_k8s.so auth_k8s.cpp \
    -I/path/to/proxysql/include \
    -lcurl
```

## Authentication Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. Client connects                                              │
│    Username: "local/proxysql/myapp"                             │
│    Password: <any - will be replaced>                           │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. ProxySQL: Initial handshake                                  │
│    Sends server greeting with default auth plugin               │
│    (mysql_native_password or caching_sha2_password)             │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. Client: Sends handshake response                             │
│    Contains hashed password (unusable for token validation)     │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. ProxySQL: Lookup user in mysql_users                         │
│    Found: attributes = '{"auth_plugin": "k8s"}'                 │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 5. ProxySQL: Check GloAuthPlugins["k8s"]                        │
│    - Not found? → Error: "Auth plugin 'k8s' not loaded"         │
│    - Found? → Continue                                          │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 6. ProxySQL: AUTH_SWITCH to mysql_clear_password                │
│    Sends: 0xFE + "mysql_clear_password" + 0x00                  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 7. Client: Resends password in clear text                       │
│    This is the K8s ServiceAccount token                         │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 8. ProxySQL: Call plugin->lookup(username, token, ...)          │
│    Plugin validates token against K8s validator service         │
└─────────────────────────────────────────────────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    ▼                       ▼
┌───────────────────────────┐ ┌───────────────────────────────────┐
│ 9a. Validation SUCCESS    │ │ 9b. Validation FAILED             │
│     - Get backend creds   │ │     - Return auth error to client │
│     - Auth complete       │ │                                   │
└───────────────────────────┘ └───────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────────────┐
│ 10. ProxySQL: Use backend credentials for connection pool       │
│     Client "local/proxysql/myapp" → Backend "app_backend_user"  │
└─────────────────────────────────────────────────────────────────┘
```

## Files Modified

| File | Change | Est. Lines |
|------|--------|------------|
| `include/proxysql_glovars.hpp` | Add `auth_plugins` field | 2 |
| `lib/ProxySQL_GloVars.cpp` | Init/cleanup `auth_plugins` | 5 |
| `src/main.cpp` | Parse config, extend `LoadPlugins()` | 50 |
| `lib/MySQL_Protocol.cpp` | Check `auth_plugin` attr, route to plugin | 40 |
| Header (new or existing) | Declare `GloAuthPlugins` map | 5 |

**Total: ~100 lines of core changes**

## Backward Compatibility

| Aspect | Behavior |
|--------|----------|
| Existing `ldap_auth_plugin` config | Unchanged, still works |
| Existing `GloMyLdapAuth` global | Unchanged, still works |
| Users without `auth_plugin` attribute | Standard password auth |
| LDAP "user not found" flow | Unchanged |
| Existing mysql_users entries | No migration needed |

## Future Enhancements

1. **Admin commands**: `LOAD AUTH PLUGIN "name"`, `SHOW AUTH PLUGINS`
2. **Runtime reload**: Reload plugins without restart
3. **Plugin variables**: Per-plugin configuration via admin interface
4. **Metrics**: Authentication stats per plugin
5. **Clear password requirement**: Let plugins declare if they need clear password via interface method

## Security Considerations

1. **Clear-text tokens**: Tokens are sent in clear after AUTH_SWITCH. TLS should be required for production use.
2. **Token validation**: Plugins should validate tokens against trusted sources (K8s API, OAuth provider).
3. **Backend credentials**: Plugins return backend credentials - these should be securely managed.
4. **Plugin trust**: Only load plugins from trusted paths. Consider plugin signing.

## References

- [ProxySQL Users Configuration](https://proxysql.com/documentation/users-configuration/)
- [ProxySQL Password Management](https://proxysql.com/documentation/password-management/)
- [MySQL Authentication Plugins](https://dev.mysql.com/doc/refman/8.0/en/pluggable-authentication.html)
- [Kubernetes ServiceAccount Tokens](https://kubernetes.io/docs/reference/access-authn-authz/authentication/#service-account-tokens)
