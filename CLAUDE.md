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
