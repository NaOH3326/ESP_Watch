ARG BASE_IMAGE=python:3.12-slim
FROM ${BASE_IMAGE}

# Mainland-China friendly defaults.  Override the base image or PyPI mirror
# with `docker compose build --build-arg ...` when deploying elsewhere.
ARG APT_MIRROR=mirrors.aliyun.com
ARG PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple
ARG PIP_TRUSTED_HOST=pypi.tuna.tsinghua.edu.cn

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_INDEX_URL=${PIP_INDEX_URL} \
    PIP_TRUSTED_HOST=${PIP_TRUSTED_HOST} \
    DATA_DIR=/data

# libgomp is needed by onnxruntime (Piper). PyAV/Pillow ship their own wheels.
# Force IPv4 because some small VPS have broken IPv6 routes to Debian mirrors.
# Switch the base image's Debian sources to a nearby mirror first; this is the
# stage that otherwise hangs for a long time on mainland-China VPSes.
RUN set -eux; \
    if [ -f /etc/apt/sources.list.d/debian.sources ]; then \
      sed -i "s|deb.debian.org|${APT_MIRROR}|g; s|security.debian.org|${APT_MIRROR}|g" /etc/apt/sources.list.d/debian.sources; \
    fi; \
    if [ -f /etc/apt/sources.list ]; then \
      sed -i "s|deb.debian.org|${APT_MIRROR}|g; s|security.debian.org|${APT_MIRROR}|g" /etc/apt/sources.list; \
    fi; \
    apt-get -o Acquire::ForceIPv4=true -o Acquire::http::Timeout=60 -o Acquire::Retries=3 update \
    && apt-get install -y --no-install-recommends libgomp1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt requirements-tts.txt ./
RUN pip install --upgrade pip \
    && pip install -r requirements.txt -r requirements-tts.txt

COPY app ./app
COPY entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

RUN mkdir -p /data/files /data/models/piper /data/tts-cache
VOLUME ["/data"]
EXPOSE 8080

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080", "--workers", "1", "--proxy-headers"]
