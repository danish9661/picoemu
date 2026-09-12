# Stage 1: Build the Go Gateway
FROM golang:1.23-alpine AS builder
WORKDIR /app
COPY go.mod ./
RUN go mod tidy
COPY . .
RUN GOOS=linux GOARCH=amd64 go build -o openhw-gw .

# Stage 2: Tiny Production Image
FROM alpine:3.19
WORKDIR /app
COPY --from=builder /app/openhw-gw .
ENV PORT=5099
EXPOSE 5099
CMD ["./openhw-gw"]
