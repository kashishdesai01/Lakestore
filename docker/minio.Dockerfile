# Upstream MinIO is source-distributed; build a pinned upstream release for local tests.
FROM golang:1.26-alpine AS build
ENV CGO_ENABLED=0 GOMAXPROCS=2 GOFLAGS=-p=2
RUN go install -ldflags='-s -w' github.com/minio/minio@RELEASE.2025-10-15T17-29-55Z
FROM alpine:3.22
RUN apk add --no-cache ca-certificates
COPY --from=build /go/bin/minio /usr/local/bin/minio
ENTRYPOINT ["minio"]
