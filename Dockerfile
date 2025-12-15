FROM debian:13
LABEL authors="Miro Stauder <miro@sysown.com>"

ARG DEB_FILE=binaries/proxysql_3.0.4-debian13_amd64.deb

RUN apt-get update && \
	apt-get install -y apt-utils

RUN apt-get install -y \
	default-mysql-client

COPY ${DEB_FILE} /tmp/proxysql.deb
RUN dpkg -i /tmp/proxysql.deb || apt-get install -fy && rm /tmp/proxysql.deb

# clean apt cache
RUN apt clean && \
	rm -rf /var/cache/apt/* && \
	rm -rf /var/lib/apt/lists/*

CMD ["proxysql", "-f", "--idle-threads", "-D", "/var/lib/proxysql"]
