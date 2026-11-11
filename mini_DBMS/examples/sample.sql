-- mini_dbms_sql data examples/sample.sql 로 실행한다.
-- id는 적지 않는다. INSERT할 때 1부터 자동으로 붙는다.
INSERT INTO users VALUES ('Kim', 'kim@example.com', 30);
INSERT INTO users VALUES ('Lee', 'lee@example.com', 25);
INSERT INTO users (name, age) VALUES ('Park', 30);

-- WHERE id = ? 는 B+ tree 인덱스로 한 줄만 읽는다.
SELECT * FROM users WHERE id = 2;
-- 다른 컬럼 조건은 파일 전체를 읽는 선형 탐색이다.
SELECT id, name FROM users WHERE age = 30;
SELECT COUNT(*) FROM users;
