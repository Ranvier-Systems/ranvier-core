#!/bin/bash
# ---------------------------------------------------------
# Docker Disk Cleanup Script for Ranvier Development
# ---------------------------------------------------------
# Run this after benchmarking or when disk usage is high.
# Safely prunes build cache and unused images without
# touching your active dev container.
#
# Usage:
#   ./scripts/docker-cleanup.sh              # Default: keep 10GB cache
#   ./scripts/docker-cleanup.sh --keep 5GB   # Custom cache limit
#   ./scripts/docker-cleanup.sh --aggressive # Remove everything unused
# ---------------------------------------------------------

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default settings
KEEP_STORAGE="${KEEP_STORAGE:-10GB}"
AGGRESSIVE=false
DRY_RUN=false

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --keep SIZE    Keep SIZE of build cache (default: 10GB)"
    echo "  --aggressive   Remove all unused images and build cache"
    echo "  --dry-run      Show what would be deleted without deleting"
    echo "  -h, --help     Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                    # Prune cache, keep 10GB"
    echo "  $0 --keep 5GB         # Prune cache, keep 5GB"
    echo "  $0 --aggressive       # Remove everything unused"
    echo "  $0 --dry-run          # Preview what would be deleted"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --keep)
            KEEP_STORAGE="$2"
            shift 2
            ;;
        --aggressive)
            AGGRESSIVE=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            exit 1
            ;;
    esac
done

echo -e "${BLUE}=== Docker Disk Cleanup for Ranvier ===${NC}"
echo ""

# Show current disk usage
echo -e "${YELLOW}Current Docker disk usage:${NC}"
docker system df
echo ""

# Get list of running containers to protect
echo -e "${YELLOW}Protected containers (currently running):${NC}"
RUNNING_CONTAINERS=$(docker ps --format "{{.Names}}" 2>/dev/null || echo "")
if [[ -n "$RUNNING_CONTAINERS" ]]; then
    echo "$RUNNING_CONTAINERS"
else
    echo "  (none)"
fi
echo ""

# Get images used by running containers
PROTECTED_IMAGES=$(docker ps --format "{{.Image}}" 2>/dev/null | sort -u || echo "")

if [[ "$DRY_RUN" == "true" ]]; then
    echo -e "${YELLOW}=== DRY RUN MODE - Nothing will be deleted ===${NC}"
    echo ""
fi

# Step 1: Prune build cache with storage limit
echo -e "${GREEN}Step 1: Pruning build cache (keeping ${KEEP_STORAGE})...${NC}"
if [[ "$DRY_RUN" == "true" ]]; then
    echo "  Would run: docker builder prune --keep-storage ${KEEP_STORAGE} -f"
else
    docker builder prune --keep-storage "${KEEP_STORAGE}" -f
fi
echo ""

# Step 2: Remove dangling images (untagged intermediate layers)
echo -e "${GREEN}Step 2: Removing dangling images...${NC}"
DANGLING=$(docker images -f "dangling=true" -q 2>/dev/null || echo "")
if [[ -n "$DANGLING" ]]; then
    if [[ "$DRY_RUN" == "true" ]]; then
        echo "  Would remove $(echo "$DANGLING" | wc -w) dangling images"
    else
        docker rmi $DANGLING 2>/dev/null || true
        echo "  Removed dangling images"
    fi
else
    echo "  No dangling images found"
fi
echo ""

# Step 3: Remove unused images (not used by any container)
# This is safe because it won't touch images used by running containers
echo -e "${GREEN}Step 3: Removing unused images (excluding protected)...${NC}"
if [[ "$AGGRESSIVE" == "true" ]]; then
    if [[ "$DRY_RUN" == "true" ]]; then
        echo "  Would run: docker image prune -a -f"
        UNUSED_IMAGES=$(docker images --format "{{.Repository}}:{{.Tag}}" | grep -v "<none>" || echo "")
        for img in $UNUSED_IMAGES; do
            if ! echo "$PROTECTED_IMAGES" | grep -q "^${img}$"; then
                echo "    Would remove: $img"
            fi
        done
    else
        docker image prune -a -f
    fi
else
    # Non-aggressive: only remove truly unused images older than 24h
    if [[ "$DRY_RUN" == "true" ]]; then
        echo "  Would run: docker image prune -a -f --filter 'until=24h'"
    else
        docker image prune -a -f --filter "until=24h" 2>/dev/null || true
    fi
fi
echo ""

# Step 4: Clean up stopped containers (except dev containers)
echo -e "${GREEN}Step 4: Removing stopped containers...${NC}"
STOPPED=$(docker ps -a -f "status=exited" --format "{{.ID}}" 2>/dev/null || echo "")
if [[ -n "$STOPPED" ]]; then
    if [[ "$DRY_RUN" == "true" ]]; then
        echo "  Would remove $(echo "$STOPPED" | wc -w) stopped containers"
    else
        docker container prune -f
        echo "  Removed stopped containers"
    fi
else
    echo "  No stopped containers found"
fi
echo ""

# Step 5: Remove unused volumes (careful - data loss possible)
echo -e "${GREEN}Step 5: Removing unused volumes...${NC}"
if [[ "$AGGRESSIVE" == "true" ]]; then
    if [[ "$DRY_RUN" == "true" ]]; then
        UNUSED_VOLS=$(docker volume ls -f "dangling=true" -q 2>/dev/null || echo "")
        if [[ -n "$UNUSED_VOLS" ]]; then
            echo "  Would remove $(echo "$UNUSED_VOLS" | wc -w) unused volumes"
        else
            echo "  No unused volumes found"
        fi
    else
        docker volume prune -f
    fi
else
    echo "  Skipped (use --aggressive to remove unused volumes)"
fi
echo ""

# Step 6: Remove unused networks
echo -e "${GREEN}Step 6: Removing unused networks...${NC}"
if [[ "$DRY_RUN" == "true" ]]; then
    echo "  Would run: docker network prune -f"
else
    docker network prune -f 2>/dev/null || true
fi
echo ""

# Show final disk usage
echo -e "${BLUE}=== Final Docker disk usage ===${NC}"
docker system df
echo ""

# Calculate and display savings
echo -e "${GREEN}Cleanup complete!${NC}"
echo ""
echo -e "${YELLOW}Tips to prevent future bloat:${NC}"
echo "  1. Use 'docker build --no-cache' only when necessary"
echo "  2. Build with Dockerfile.production.fast after setting up ranvier-base"
echo "  3. Run this script weekly or after major benchmark sessions"
echo "  4. Consider using: export DOCKER_BUILDKIT=1"
