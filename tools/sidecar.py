#!/usr/bin/env python

import docker
import requests
import time
import os

# Configuration
DOCKER_SOCK = "unix://var/run/docker.sock"
RANVIER_API = "http://localhost:8080/admin/backends"

def get_container_ip(container):
    """Extracts the IP address from a container object."""
    # This varies by network mode, but usually it's under 'bridge' or the default network
    settings = container.attrs['NetworkSettings']
    # Try to find the first IP address available
    for net_name, net_conf in settings['Networks'].items():
        if net_conf['IPAddress']:
            return net_conf['IPAddress']
    return None

def sync_backends(client):
    """Finds labeled containers and registers them with Ranvier."""
    print("🔍 Scanning for Ranvier backends...")
    
    # 1. Find Containers with the magic label
    containers = client.containers.list(filters={"label": "ranvier.backend=true"})
    
    if not containers:
        print("   No backends found.")
        return

    for c in containers:
        try:
            # 2. Extract Config from Labels
            # Defaults: ID=Last 4 chars of Container ID, Port=8000
            backend_id = c.labels.get("ranvier.id", str(int(c.short_id, 16) % 1000))
            port = c.labels.get("ranvier.port", "8000")
            
            # 3. Get IP
            ip = get_container_ip(c)
            if not ip:
                print(f"   ⚠️  Container {c.name} has no IP. Skipping.")
                continue

            # 4. Register with Ranvier
            payload = {
                "id": backend_id,
                "ip": ip,
                "port": port
            }
            
            # We send this every loop. Ranvier (in production) should be idempotent (handle duplicates gracefully).
            # For now, we just overwrite.
            resp = requests.post(f"{RANVIER_API}?id={backend_id}&ip={ip}&port={port}")
            
            if resp.status_code == 200:
                print(f"   ✅ Registered: {c.name} -> {ip}:{port} (ID: {backend_id})")
            else:
                print(f"   ❌ Failed to register {c.name}: {resp.text}")

        except Exception as e:
            print(f"   Error processing {c.name}: {e}")

if __name__ == "__main__":
    # Ensure we can talk to Docker
    try:
        client = docker.from_env()
        print("🔌 Connected to Docker Daemon.")
    except Exception as e:
        print(f"🚨 Could not connect to Docker: {e}")
        print("   (Did you mount /var/run/docker.sock?)")
        exit(1)

    # Main Loop
    while True:
        try:
            sync_backends(client)
        except Exception as e:
            print(f"   Sync Error: {e}")
        
        time.sleep(5)
