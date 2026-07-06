FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY relay.py .

ENV PYTHONUNBUFFERED=1
ENV METRICS_PORT=9101

# Run as a non-root user. The container only needs to write to /data (the JSONL
# log volume); granting least privilege limits the blast radius if a
# vulnerability in requests/paho-mqtt is exploited via a malicious MQTT payload.
RUN useradd --system --no-create-home --uid 1001 relay \
    && mkdir -p /data && chown -R relay:relay /data /app
USER relay

EXPOSE 9101

# Health check hits the in-process metrics HTTP server on /health.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:9101/health', timeout=3).status == 200 else 1)"

CMD ["python", "relay.py"]
