# ProxySQL Study Notes for mariadb-auth-k8s Integration

## Overview

This document captures research findings on integrating ProxySQL with the mariadb-auth-k8s authentication plugin, which uses `mysql_clear_password` to pass Kubernetes ServiceAccount JWT tokens.

## ProxySQL Authentication Architecture

### Two-Phase Authentication

ProxySQL performs authentication in two phases:

1. **Frontend Authentication**: Client → ProxySQL
2. **Backend Authentication**: ProxySQL → MySQL/MariaDB

```
Client ──(1)──> ProxySQL ──(2)──> MariaDB
         │              │
         │              └── Backend auth (ProxySQL as client)
         └── Frontend auth (ProxySQL as server)
```

### Authentication Plugin Enum

From `include/MySQL_Protocol.h`:

```c
enum proxysql_auth_plugins {
    AUTH_MYSQL_NATIVE_PASSWORD = 0,
    AUTH_MYSQL_CLEAR_PASSWORD = 1,      // index 1
    AUTH_MYSQL_CACHING_SHA2_PASSWORD = 2
};
```

### Plugin String Array

From `lib/MySQL_Protocol.cpp`:

```c
static const char *plugins[3] = {
    "mysql_native_password",   // index 0
    "mysql_clear_password",    // index 1
    "caching_sha2_password"    // index 2
};
```

## Default Authentication Plugin Limitation

### Original Code

In `lib/MySQL_Thread.cpp`, ProxySQL only accepts two values for `default_authentication_plugin`:

```c
const char * valids[2] = { "mysql_native_password", "caching_sha2_password" };
for (long unsigned int i=0; i < sizeof(valids)/sizeof(char *) ; i++) {
    if (strcmp(valids[i],value)==0) {
        // ...
        if (i==0) variables.default_authentication_plugin_int = 0;
        if (i==1) variables.default_authentication_plugin_int = 2;  // Note: maps to 2, not 1
        return true;
    }
}
proxy_error("%s is an invalid value for default_authentication_plugin\n", value);
```

### Handshake Assertion

In `lib/MySQL_Protocol.cpp`, the handshake generation asserts only two valid plugin IDs:

```c
bool MySQL_Protocol::generate_pkt_initial_handshake(...) {
    int use_plugin_id = mysql_thread___default_authentication_plugin_int;
    assert(use_plugin_id == 0 || use_plugin_id == 2);  // Crashes if plugin_id == 1
    // ...
}
```

### Changes Required

To enable `mysql_clear_password` as default (already applied):

1. Add to valid values array in `MySQL_Thread.cpp`
2. Map index to correct plugin_int (1)
3. Update assertion in `MySQL_Protocol.cpp`

## LDAP Authentication Flow

ProxySQL supports `mysql_clear_password` through LDAP integration for **unknown users**.

### How LDAP Auth Works

From `lib/MySQL_Protocol.cpp` - `PPHR_4auth1()`:

```c
bool MySQL_Protocol::PPHR_4auth1(...) {
    if (GloMyLdapAuth) {  // Only if LDAP plugin is loaded
        if ((*myds)->switching_auth_stage == 0) {
            user_exists = GloMyAuth->exists((char *)vars1.user);
            if (user_exists == false) {
                // User NOT in mysql_users table
                (*myds)->switching_auth_type = AUTH_MYSQL_CLEAR_PASSWORD;
                (*myds)->switching_auth_stage = 1;
                (*myds)->auth_in_progress = 1;
                generate_pkt_auth_switch_request(true, NULL, NULL);
                // ...
            }
        }
    }
    return true;
}
```

### LDAP Authentication Sequence

```
1. Client connects with mysql_native_password (default)
2. ProxySQL checks if user exists in mysql_users
3. If user NOT found AND LDAP enabled:
   a. ProxySQL sends AuthSwitchRequest to mysql_clear_password
   b. Client sends cleartext password
   c. ProxySQL calls GloMyLdapAuth->lookup() to validate
4. If LDAP validates, authentication succeeds
```

### LDAP Plugin Interface

From `include/MySQL_LDAP_Authentication.hpp`:

```cpp
class MySQL_LDAP_Authentication {
public:
    virtual ~MySQL_LDAP_Authentication() {};

    // Main authentication method
    virtual char* lookup(
        char *username,
        char *pass,                    // Cleartext password (JWT token in our case)
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
    ) { return NULL; }

    virtual int increase_frontend_user_connections(char *username, int *max_connections = NULL) { return 0; }
    virtual void decrease_frontend_user_connections(char *username) {}

    // Variable management
    virtual char **get_variables_list() { return NULL; }
    virtual bool has_variable(const char *name) { return false; }
    virtual char* get_variable(char *name) { return NULL; }
    virtual bool set_variable(char *name, char *value) { return false; }

    // ...
};

typedef MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_t();
```

### Loading LDAP Plugin

From `src/main.cpp`:

```c
if (GloVars.ldap_auth_plugin) {
    __mysql_ldap_auth = dlopen(GloVars.ldap_auth_plugin, RTLD_NOW);
    create_MySQL_LDAP_Authentication = (create_MySQL_LDAP_Authentication_t*)
        dlsym(__mysql_ldap_auth, "create_MySQL_LDAP_Authentication_func");
    GloMyLdapAuth = create_MySQL_LDAP_Authentication();
}
```

Configuration:
```
# proxysql.cnf
ldap_auth_plugin="/path/to/plugin.so"
```

## Frontend Authentication Details

### Password Validation Flow

When a client authenticates, ProxySQL:

1. Receives HandshakeResponse with auth data
2. Looks up user in `mysql_users` table
3. Compares received password hash against stored password
4. If match, proceeds to backend connection

### The Problem with JWT Tokens

For mariadb-auth-k8s:
- Password = JWT token (dynamic, per-connection)
- Cannot pre-configure in `mysql_users`
- ProxySQL cannot validate JWT without custom logic

### fast_forward Mode

Setting `fast_forward=1` in `mysql_users`:
- Skips query parsing and rewriting
- Does NOT skip frontend authentication
- User must still authenticate to ProxySQL first

## Backend Authentication

### How ProxySQL Authenticates to Backend

From `lib/mysql_connection.cpp`:

ProxySQL creates backend connections using credentials from:
1. `mysql_users.backend_username` (if set)
2. `mysql_users.username` (default)
3. Password from `mysql_users.password`

### Cleartext Password to Backend

When ProxySQL connects to backend with `mysql_clear_password`:
- It uses the stored password from `mysql_users`
- NOT the password received from client
- This breaks JWT passthrough

## Integration Approaches

### Approach 1: Custom LDAP Plugin

Create a shared library implementing `MySQL_LDAP_Authentication`:

```cpp
extern "C" MySQL_LDAP_Authentication* create_MySQL_LDAP_Authentication_func() {
    return new K8s_Auth_Plugin();
}

class K8s_Auth_Plugin : public MySQL_LDAP_Authentication {
public:
    char* lookup(char *username, char *pass, ...) override {
        // 1. Parse username: cluster/namespace/serviceaccount
        // 2. Validate JWT token via TokenReview or JWKS
        // 3. Return password to use for backend (same JWT)
        // 4. Set default_hostgroup, etc.
    }
};
```

**Pros:**
- Uses existing ProxySQL mechanism
- No core ProxySQL modifications needed (beyond the patch)

**Cons:**
- Complex to implement
- Must handle backend auth separately
- Plugin API may change between versions

### Approach 2: Passthrough Authentication Patch

Modify ProxySQL to skip frontend validation when configured:

1. Add config option: `mysql-auth_passthrough=true`
2. In `PPHR_5passwordFalse_auth2()`, accept any cleartext password
3. Store received password for backend connection
4. Forward same password to backend

**Files to modify:**
- `lib/MySQL_Protocol.cpp` - Accept any password
- `lib/mysql_connection.cpp` - Use client's password for backend
- `lib/MySQL_Thread.cpp` - Add configuration variable

### Approach 3: Pre-registered Users with Empty Password

```sql
INSERT INTO mysql_users (username, password, default_hostgroup, fast_forward)
VALUES
    ('local/mariadb-auth-test/user1', '', 0, 1),
    ('local/mariadb-auth-test/user2', '', 0, 1),
    ('cluster-b/remote-test/remote-user', '', 0, 1);
```

With patch applied and empty password:
- ProxySQL accepts any cleartext password for these users
- Need to verify if password is forwarded to backend

## Key Source Files

| File | Purpose |
|------|---------|
| `lib/MySQL_Thread.cpp` | Variable handling, thread management |
| `lib/MySQL_Protocol.cpp` | Protocol implementation, handshake, auth |
| `lib/mysql_connection.cpp` | Backend connection management |
| `lib/MySQL_Session.cpp` | Session state, auth flow coordination |
| `include/MySQL_Protocol.h` | Protocol enums and class definition |
| `include/MySQL_LDAP_Authentication.hpp` | LDAP plugin interface |
| `src/main.cpp` | Plugin loading, initialization |

## References

- ProxySQL GitHub: https://github.com/sysown/proxysql
- Build & Test Instructions: See `CLAUDE.md`
- mariadb-auth-k8s: https://github.com/rophy/mariadb-auth-k8s
- kube-federated-auth: https://github.com/rophy/kube-federated-auth
