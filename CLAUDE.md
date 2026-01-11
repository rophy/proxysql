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

The `k8s/` directory contains manifests for testing ProxySQL with Kubernetes ServiceAccount authentication.

Architecture:
```
test-client (SA token) → ProxySQL (k8s auth plugin) → MariaDB (standard)
                              ↓
                    K8s TokenReview API
```

ProxySQL's k8s auth plugin validates ServiceAccount JWT tokens directly via the Kubernetes TokenReview API.

### Prerequisites

Build ProxySQL and the k8s auth plugin before deploying:

```bash
# Build ProxySQL
docker compose exec dev make

# Build k8s auth plugin
docker compose exec dev make -C /proxysql/plugins/MySQL_AuthPlugin/k8s
```

### Deploy with Skaffold

Requires: `kind` cluster named `cluster-a` and `skaffold` CLI.

```bash
# Create kind cluster (if not exists)
kind create cluster --name cluster-a

# Deploy all components
skaffold run
```

This deploys to namespace `proxysql`:
- ProxySQL with k8s auth plugin
- MariaDB (standard image)
- test-client (Debian pod with mysql-client)

### Test Authentication

Connect from test-client using ServiceAccount token as password:

```bash
kubectl exec -n proxysql deployment/test-client -- bash -c '
TOKEN=$(cat /var/run/secrets/kubernetes.io/serviceaccount/token)
mysql -h proxysql -P 6033 -u k8s-user -p"$TOKEN" -e "SELECT CURRENT_USER();"
'
```

Expected result: `dbuser@%` (the backend user mapped from the k8s-user frontend user).
