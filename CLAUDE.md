# ProxySQL Development

**All development activity (building, testing, debugging) must be done inside the dev container.**

## Dev Container

Start the dev container:
```bash
docker compose up -d
```

Build ProxySQL inside the container:
```bash
docker compose exec dev make
```

Stop the container:
```bash
docker compose down
```

## Local Testing with MariaDB

The `docker-compose.yaml` includes both the dev container and a MariaDB 11.4 instance for local testing.

Services:
- **dev**: ProxySQL build environment with source mounted at `/proxysql`
- **mariadb**: MariaDB 11.4 with root password `root` and database `testdb`

Test MariaDB connectivity from dev container:
```bash
docker compose exec dev mysql -h mariadb -u root -proot -e "SELECT VERSION()"
```

Create test users:
```bash
docker compose exec mariadb mariadb -u root -proot -e "
CREATE USER 'testuser'@'%' IDENTIFIED BY 'testpass';
GRANT ALL ON testdb.* TO 'testuser'@'%';
FLUSH PRIVILEGES;
"
```

## Build Docker Image

Build a ProxySQL Docker image from local source:
```bash
./scripts/build-docker.sh
```

This script:
1. Builds the deb package using `docker-compose-build.yaml`
2. Creates a Docker image with the local package

Customize the build with environment variables:
```bash
CURVER=3.0.4 PKG_RELEASE=debian13 IMAGE_TAG=proxysql:latest ./scripts/build-docker.sh
```

## Kubernetes Authentication Testing

### Overview

The `k8s/` directory contains manifests for testing MariaDB with Kubernetes ServiceAccount authentication:

- **mariadb-auth-k8s**: A MariaDB authentication plugin that validates Kubernetes ServiceAccount tokens instead of passwords. Users authenticate with their SA token, and MariaDB verifies it against the cluster's OIDC endpoint.

- **kube-federated-auth**: A Go service that validates ServiceAccount tokens across Kubernetes clusters. MariaDB's auth plugin calls its `/validate` endpoint to verify tokens.

### Deploy with Skaffold

Requires: `kind` cluster named `cluster-a` and `skaffold` CLI.

```bash
# Create kind cluster (if not exists)
kind create cluster --name cluster-a

# Deploy all components
skaffold run

# Or with live reload for development
skaffold dev
```

This deploys to namespace `proxysql`:
- kube-federated-auth (token validation service)
- MariaDB with auth_k8s plugin
- test-client (Debian pod with mysql-client)

### Initialize MariaDB Users

After deployment, create users that authenticate via ServiceAccount tokens:

```bash
kubectl exec -n proxysql deployment/mariadb -- mariadb -u root -e "
CREATE USER 'local/proxysql/testuser'@'%' IDENTIFIED VIA auth_k8s;
GRANT ALL ON testdb.* TO 'local/proxysql/testuser'@'%';
FLUSH PRIVILEGES;
"
```

Username format: `local/<namespace>/<serviceaccount>`

### Test Authentication

Connect from test-client using ServiceAccount token as password:

```bash
kubectl exec -n proxysql deployment/test-client -- bash -c '
TOKEN=$(cat /var/run/secrets/kubernetes.io/serviceaccount/token)
mariadb -h mariadb -u "local/proxysql/testuser" -p"$TOKEN" -e "SELECT CURRENT_USER();"
'
```
