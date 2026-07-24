FROM python:3.12-slim
WORKDIR /app
COPY contact.py .
ENV PORT=8484 CONTACT_FILE=/data/contact.jsonl
VOLUME /data
EXPOSE 8484
CMD ["python3", "contact.py"]
