FROM nginx:1.27-alpine

COPY nginx.conf /etc/nginx/nginx.conf
COPY site/ /usr/share/nginx/html/

RUN nginx -t

EXPOSE 80
