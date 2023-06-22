# https://antonz.org/sql-fetch/

CREATE TEMPORARY TABLE employees (id UInt64, name String, department String, salary UInt64);
INSERT INTO employees VALUES (23, 'Henry', 'it', 104), (24, 'Irene', 'it', 104), (25, 'Frank', 'it', 120), (31, 'Cindy', 'sales', 96), (33, 'Alice', 'sales', 100), (32, 'Dave', 'sales', 96), (22, 'Grace', 'it', 90), (21, 'Emma', 'it', '84');

select * from employees
order by salary desc
limit 5
format PrettyCompactNoEscapes;

select * from employees
order by salary desc
fetch first 5 rows only
format PrettyCompactNoEscapes;

select * from employees
order by salary desc
fetch first 5 rows with ties
format PrettyCompactNoEscapes;

select * from employees
order by salary desc
offset 3 rows
fetch next 5 rows only
format PrettyCompactNoEscapes;

select * from employees
order by salary desc
offset 3 rows
fetch first 5 rows only
format PrettyCompactNoEscapes;
