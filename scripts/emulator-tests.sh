#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export LAKESTORE_S3_ENDPOINT=http://127.0.0.1:19000
export LAKESTORE_AZURE_ENDPOINT=http://127.0.0.1:11000/devstoreaccount1
export AWS_ACCESS_KEY_ID=lakestore
export AWS_SECRET_ACCESS_KEY=lakestore-local-password
export AZURE_STORAGE_ACCOUNT=devstoreaccount1
export AZURE_STORAGE_KEY=Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==
build_dir="${1:-build}"
for attempt in $(seq 1 30); do
  if "$build_dir/lakestore" init-store --store s3://lakestore-tests >/dev/null 2>&1 && "$build_dir/lakestore" init-store --store azure://lakestore-tests >/dev/null 2>&1; then
    "$build_dir/lakestore_cloud_tests"
    exit 0
  fi
  sleep 1
done
printf '%s\n' 'Emulators did not become ready' >&2
exit 1
