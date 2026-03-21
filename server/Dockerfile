FROM python:3.12-slim

WORKDIR /app
COPY server/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY server/app ./app
COPY server/write_bridge_config.py ./server/write_bridge_config.py
COPY server/bridge_sidecar.py ./server/bridge_sidecar.py
COPY server/docker-entrypoint.sh ./server/docker-entrypoint.sh
COPY bridge ./bridge
COPY .env.example ./.env.example

RUN chmod +x /app/server/docker-entrypoint.sh

ENV SAVE_ROOT=/data/saves
ENV HISTORY_ROOT=/data/history
ENV INDEX_PATH=/data/index.json
ENV ENABLE_VERSION_HISTORY=true
ENV PYTHONPATH=/app/bridge

EXPOSE 8080

ENTRYPOINT ["/app/server/docker-entrypoint.sh"]
