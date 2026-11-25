#!/bin/sh

cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-./build/nexcent}
[ -x "$BIN" ] || { echo "Спочатку зберіть сервер: make"; exit 1; }

PORT=${TEST_PORT:-3199}
BASE=http://127.0.0.1:$PORT
TMP=$(mktemp -d)
pass=0
fail=0

DB_PATH=$TMP/test.db PORT=$PORT ADMIN_EMAIL=admin@test.local ADMIN_PASSWORD='AdminPass123!' $BIN >"$TMP/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$TMP"' EXIT
i=0
until curl -s -o /dev/null "$BASE/api/health"; do
    i=$((i + 1))
    [ $i -gt 50 ] && { echo "Сервер не запустився:"; cat "$TMP/server.log"; exit 1; }
    sleep 0.2
done

req() {
    set -- -s -o "$TMP/body" -w '%{http_code}' -X "$1" "$BASE$2" ${3:+-H "Authorization: Bearer $3"} ${4:+-H 'Content-Type: application/json' --data "$4"}
    STATUS=$(curl "$@")
    BODY=$(cat "$TMP/body")
}
check() {
    if [ "$STATUS" = "$2" ] && { [ -z "$3" ] || case "$BODY" in *"$3"*) true ;; *) false ;; esac; }; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL: $1 — очікувалось $2 '$3', отримано $STATUS: $BODY"
    fi
}
token() { printf '%s' "$BODY" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p'; }

echo "== Публічні"
req GET /api/health;                         check health 200 '"status":"ok"'
req GET '/api/posts?limit=2';                check posts 200 '"total":6'
req GET /api/posts/five-ways-to-grow-your-membership-income; check post 200 '"body"'
req GET /api/posts/nope;                     check post-404 404
req GET /api/faqs;                           check faqs 200 '"question"'
req POST /api/subscribe '' '{"email":"s@x.com"}';   check subscribe 200 '"ok":true'
req POST /api/subscribe '' '{"email":"s@x.com"}';   check subscribe-повтор 200
req POST /api/subscribe '' '{"email":"bad"}';       check subscribe-bad 422 'valid email'
req POST /api/contact '' '{"name":"Боб","email":"bob@x.com","message":"Привіт!"}'; check contact 201
req POST /api/contact '' '{"name":"","email":"x","message":""}';  check contact-bad 422 'Name must be'
req POST /api/contact '' 'not json';         check contact-json 400
req DELETE /api/posts;                       check 405 405
req GET /api/nothing;                        check 404 404

echo "== Реєстрація і вхід"
req POST /api/register '' '{"name":"Ann","email":"ann@x.com","password":"123"}'; check register-short 422 '8–128'
req POST /api/register '' '{"name":"Ann Lee","email":"Ann@X.com","password":"secret123"}'; check register 201 '"email":"ann@x.com"'
USER=$(token)
req POST /api/register '' '{"name":"Ann","email":"ann@x.com","password":"secret123"}'; check register-dup 409 'already exists'
req POST /api/login '' '{"email":"ann@x.com","password":"wrongwrong"}'; check login-bad 401 'Invalid email'
req POST /api/login '' '{"email":"nobody@x.com","password":"whatever1"}'; check login-unknown 401
req POST /api/login '' '{"email":"ann@x.com","password":"secret123"}'; check login 200 '"token"'
USER=$(token)
req GET /api/me;                              check me-без-токена 401
req GET /api/me "bad-token";                  check me-поганий-токен 401
req GET /api/me "$USER";                      check me 200 '"role":"user"'

echo "== Права доступу"
req GET /api/admin/stats "$USER";             check user-не-адмін 403
req GET /api/admin/stats;                     check без-токена 401
req POST /api/admin/posts "$USER" '{"title":"Hack"}'; check user-post 403

echo "== Адміністратор"
req POST /api/login '' '{"email":"admin@test.local","password":"AdminPass123!"}'; check admin-login 200
ADMIN=$(token)
req GET /api/admin/stats "$ADMIN";            check stats 200 '"counts"'
case "$BODY" in *'"workers":[{'*'"server":{'*) pass=$((pass + 1)) ;; *) fail=$((fail + 1)); echo "  FAIL: stats без workers/server" ;; esac
req GET '/api/admin/benchmark?n=2' "$ADMIN";  check benchmark 200 '"per_thread_ms"'
req GET /api/admin/audit "$ADMIN";            check audit 200 '"login_fail"'

echo "== Повідомлення, підписники, користувачі"
req GET /api/admin/messages "$ADMIN";         check messages 200 '"total":1'
req GET '/api/admin/messages?unread=1' "$ADMIN"; check messages-unread 200 '"is_read":0'
req PATCH /api/admin/messages/1 "$ADMIN" '{"is_read":true}'; check message-read 200
req GET '/api/admin/messages?unread=1' "$ADMIN"; check messages-unread-0 200 '"total":0'
req PATCH /api/admin/messages/1 "$ADMIN" '{"is_read":"yes"}'; check message-bad 422
req PATCH /api/admin/messages/999 "$ADMIN" '{"is_read":true}'; check message-404 404
req DELETE /api/admin/messages/1 "$ADMIN";    check message-delete 200
req GET /api/admin/subscribers "$ADMIN";      check subscribers 200 's@x.com'
req DELETE /api/admin/subscribers/1 "$ADMIN"; check subscriber-delete 200
req GET /api/admin/users "$ADMIN";            check users 200 '"total":2'
req PATCH /api/admin/users/2 "$ADMIN" '{"role":"boss"}'; check role-bad 422
req PATCH /api/admin/users/1 "$ADMIN" '{"role":"user"}'; check role-self 422 'your own'
req DELETE /api/admin/users/1 "$ADMIN";       check delete-self 422 'your own'
req PATCH /api/admin/users/2 "$ADMIN" '{"role":"admin"}'; check role-change 200
req GET /api/admin/stats "$USER";             check role-діє-одразу 200
req PATCH /api/admin/users/2 "$ADMIN" '{"role":"user"}'; check role-back 200

echo "== Пости (CRUD)"
req POST /api/admin/posts "$ADMIN" '{"title":"ab"}'; check post-коротка-назва 422 'Title'
req POST /api/admin/posts "$ADMIN" '{"title":"Hello, World!","body":"First\n\nSecond","cover":"http://evil/x.jpg"}'; check post-cover 422 'Cover'
req POST /api/admin/posts "$ADMIN" '{"title":"Hello, World!","excerpt":"E","body":"First\n\nSecond","published":false}'; check post-create 201 '"slug":"hello-world"'
req POST /api/admin/posts "$ADMIN" '{"title":"Hello, World!"}'; check post-дубль-slug 201
req GET /api/posts/hello-world;               check чернетка-прихована 404
req PUT /api/admin/posts/7 "$ADMIN" '{"title":"Hello again","body":"New","author":"Me","published":true}'; check post-update 200
req GET /api/posts/hello-world;               check опублікований 200 '"author":"Me"'
req GET /api/admin/posts/7 "$ADMIN";          check admin-get-post 200 '"published":1'
req GET /api/admin/posts "$ADMIN";            check admin-list-posts 200 '"total":8'
req DELETE /api/admin/posts/7 "$ADMIN";       check post-delete 200
req DELETE /api/admin/posts/7 "$ADMIN";       check post-delete-2 404

echo "== Вихід"
req POST /api/logout "$USER";                 check logout 200
req GET /api/me "$USER";                      check токен-відкликано 401
req DELETE /api/admin/users/2 "$ADMIN";       check user-delete 200
req GET /api/admin/audit "$ADMIN";            check audit-події 200 '"role_change"'

echo "== Статичні файли"
for p in / /index.html /pages/login.html /css/style.css /js/main.js /images/blog-1.jpg; do
    req GET "$p"; check "static $p" 200
done
for p in /server/server.c /database/schema.sql /Makefile /.gitignore /pages/../server/server.c /pages/%2e%2e/server/server.c //etc/passwd /pages/; do
    req GET "$p"; check "закрито $p" 404
done
req POST / '' '{}';                           check static-POST 405

echo
echo "Пройшло: $pass, провалилось: $fail"
[ "$fail" -eq 0 ]
