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
