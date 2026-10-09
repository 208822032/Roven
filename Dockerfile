FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       liburing2 bash ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/kvlite

COPY kvstore /opt/kvlite/kvstore
COPY config /opt/kvlite/config
COPY models /opt/kvlite/models
COPY third_party/onnxruntime/lib /opt/kvlite/third_party/onnxruntime/lib

RUN chmod +x /opt/kvlite/kvstore \
    && mkdir -p /opt/kvlite/data

EXPOSE 28080 9300 9080

ENTRYPOINT ["/opt/kvlite/kvstore"]
