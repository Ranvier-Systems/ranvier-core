#!/usr/bin/env bash

# DELETE /admin/backends?id=X          - Remove backend and its routes
curl -X DELETE "http://localhost:8080/admin/backends?id=91"
echo ''

# DELETE /admin/routes?backend_id=X    - Remove routes for a specific backend
#curl -X DELETE "http://localhost:8080/admin/routes?backend_id=91"
#echo ''

# POST   /admin/clear                  - Wipe all persisted data (dangerous)
#curl -X POST "http://localhost:8080/admin/clear"
#echo ''
