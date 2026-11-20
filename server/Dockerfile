FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1 \
    DATA_DIR=/data

# libgomp is needed by onnxruntime (Piper). PyAV/Pillow ship their own wheels.
# Force IPv4 because some small VPS have broken IPv6 routes to Debian mirrors.
RUN apt-get -o Acquire::ForceIPv4=true -o Acquire::http::Timeout=60 update \
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
