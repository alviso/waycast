FROM python:3.12-slim
WORKDIR /app
COPY tileserver.py .
ENV PORT=8484 CACHE_DIR=/data/cache
VOLUME /data
EXPOSE 8484
CMD ["python3", "tileserver.py"]
