#!/usr/bin/env python3
"""Run bounded real-service smoke tests and clean only this run's newly-created objects.

AWS uses an explicitly selected CLI profile, without printing its exported credentials.
Azure reads locally configured environment credentials. Existing namespaces need a unique prefix.
"""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile
import uuid
from datetime import datetime, timezone


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", choices=["s3", "azure"], required=True)
    parser.add_argument("--profile", default="default")
    parser.add_argument("--region", default="us-east-1")
    parser.add_argument("--container", help="existing bucket/container; never removed")
    parser.add_argument("--binary", default="build/lakestore")
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    binary = str(pathlib.Path(args.binary).resolve())
    environment = dict(os.environ)
    environment.pop("LAKESTORE_S3_ENDPOINT", None)
    environment.pop("LAKESTORE_AZURE_ENDPOINT", None)
    run_id = uuid.uuid4().hex
    namespace = args.container or f"lakestore-validation-{run_id}"
    prefix = f"validation-{run_id}"
    created = False
    touched = False
    passed = []
    result = {"provider": args.provider, "namespace": namespace, "prefix": prefix,
              "started_utc": datetime.now(timezone.utc).isoformat(), "checks": passed,
              "cleanup": False, "status": "failed"}

    def aws(*arguments):
        return subprocess.run(["aws", *arguments, "--profile", args.profile, "--region", args.region],
                              check=True, text=True, capture_output=True, timeout=90).stdout

    def cli(*arguments, expired=False):
        completed = subprocess.run([binary, *arguments, "--store", uri], env=environment,
                                   capture_output=True, text=True, timeout=90)
        if expired:
            # ErrorCode::Expired is 9; unrelated service errors must fail verification.
            error = json.loads(completed.stderr)
            if completed.returncode != 1 or error.get("error") != 9:
                raise RuntimeError(f"expected typed Expired rejection: {completed.stderr}")
            return None
        if completed.returncode:
            raise RuntimeError(f"Lakestore {arguments[0]} failed: {completed.stderr}")
        return json.loads(completed.stdout)

    uri = f"{args.provider}://{namespace}/{prefix}"
    try:
        if args.provider == "s3":
            credentials = json.loads(aws("configure", "export-credentials", "--format", "process"))
            environment["AWS_ACCESS_KEY_ID"] = credentials["AccessKeyId"]
            environment["AWS_SECRET_ACCESS_KEY"] = credentials["SecretAccessKey"]
            if credentials.get("SessionToken"):
                environment["AWS_SESSION_TOKEN"] = credentials["SessionToken"]
            else:
                environment.pop("AWS_SESSION_TOKEN", None)
            environment["AWS_REGION"] = args.region
            if not args.container:
                options = ["s3api", "create-bucket", "--bucket", namespace]
                if args.region != "us-east-1":
                    options += ["--create-bucket-configuration", f"LocationConstraint={args.region}"]
                aws(*options)
                created = True
                aws("s3api", "put-public-access-block", "--bucket", namespace,
                    "--public-access-block-configuration",
                    "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true")
        else:
            if not environment.get("AZURE_STORAGE_ACCOUNT") or not (
                    environment.get("AZURE_STORAGE_KEY") or environment.get("AZURE_STORAGE_BEARER_TOKEN")):
                raise RuntimeError("Azure credentials are not configured locally")
            if not args.container:
                raise RuntimeError("Azure real-service tests require an existing --container")
        touched = True
        cli("create", "--table", "t", "--schema", "id:int64,label:string")
        with tempfile.TemporaryDirectory(prefix="lakestore-cloud-") as tmp:
            csv = pathlib.Path(tmp) / "rows.csv"
            csv.write_text("id,label\n1,one\n2,two\n")
            version = cli("append", "--table", "t", "--input", str(csv))["version"]
            assert cli("scan", "--table", "t")["rows"] == [[1, "one"], [2, "two"]]
            passed.append("append-and-ranged-scan")
            assert cli("scan", "--table", "t", "--where", "id = 1")["rows"] == [[1, "one"]]
            passed.append("predicate-scan")
            cli("clone", "--source", "t", "--version", str(version), "--as", "clone")
            csv.write_text("id,label\n3,three\n")
            cli("overwrite", "--table", "t", "--input", str(csv))
            assert cli("scan", "--table", "clone")["rows"] == [[1, "one"], [2, "two"]]
            passed.append("zero-copy-clone-divergence")
            cli("restore", "--table", "t", "--version", str(version))
            assert cli("scan", "--table", "t")["rows"] == [[1, "one"], [2, "two"]]
            passed.append("time-travel-and-restore")
            streamed = subprocess.run([binary, "scan", "--table", "t", "--stream", "--store", uri],
                                      env=environment, capture_output=True, text=True, check=True, timeout=90)
            records = [json.loads(line) for line in streamed.stdout.splitlines()]
            assert records[-1]["event"] == "scan_complete" and records[-1]["rows"] == 2
            passed.append("streaming-scan")
            cli("expire-snapshots", "--table", "t", "--keep-last", "1")
            cli("scan", "--table", "t", "--version", str(version), expired=True)
            cli("gc", "--offline", "--grace", "0ms")
            cli("gc", "--offline", "--grace", "0ms")
            cli("verify", "--table", "t")
            cli("verify", "--table", "clone")
            passed.append("durable-retention-and-offline-gc")
        result["status"] = "passed"
    except Exception as error:
        # CLI stderr contains typed errors, never exported authentication values.
        result["error"] = str(error)
        raise
    finally:
        try:
            if args.provider == "s3" and (created or args.container):
                aws("s3", "rm", f"s3://{namespace}/{prefix}/", "--recursive", "--only-show-errors")
                if created:
                    aws("s3api", "delete-bucket", "--bucket", namespace)
                result["cleanup"] = True
            elif args.provider == "azure" and args.container and touched:
                # A dedicated cleanup utility below deletes only this exact random prefix.
                cleanup = subprocess.run([str(pathlib.Path(binary).with_name("lakestore_cleanup")),
                                          namespace, prefix], env=environment,
                                         capture_output=True, text=True, timeout=90)
                result["cleanup"] = cleanup.returncode == 0
        finally:
            result["finished_utc"] = datetime.now(timezone.utc).isoformat()
            pathlib.Path(args.report).write_text(json.dumps(result, indent=2) + "\n")
    if not result["cleanup"]:
        raise RuntimeError("test cleanup incomplete; inspect report")


if __name__ == "__main__":
    main()
