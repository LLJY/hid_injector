mod error;

use actix_web::{delete, get, post,put, web, App, HttpResponse, HttpServer, Responder};
use dotenvy::dotenv;
use error::AppError;
use serde::{Deserialize, Serialize};
use sqlx::{FromRow, SqlitePool};
use std::env;

#[derive(Serialize, FromRow, Debug)]
struct Task {
    id: i64,
    name: String,
    description: Option<String>,
}

#[derive(Deserialize)]
struct CreateTask {
    name: String,
    description: Option<String>,
}

#[get("/tasks")]
async fn get_tasks(db_pool: web::Data<SqlitePool>) -> Result<impl Responder, AppError> {
    let tasks = sqlx::query_as::<_, Task>("SELECT id, name, description FROM tasks")
        .fetch_all(db_pool.get_ref())
        .await
        .map_err(AppError::Database)?;
    println!("Fetched {} tasks", tasks.len());
    Ok(HttpResponse::Ok().json(tasks))
}

#[get("/tasks/{id}")]
async fn get_task_by_id(
    db_pool: web::Data<SqlitePool>,
    path: web::Path<i64>,
) -> Result<impl Responder, AppError> {
    let id = path.into_inner();
    let task = sqlx::query_as::<_, Task>("SELECT id, name, description FROM tasks WHERE id = ?")
        .bind(id)
        .fetch_optional(db_pool.get_ref())
        .await
        .map_err(AppError::Database)?;

    match task {
        Some(t) => Ok(HttpResponse::Ok().json(t)),
        None => Err(AppError::NotFound(format!("Task with id {} not found", id))),
    }
}

#[post("/tasks")]
async fn create_task(
    db_pool: web::Data<SqlitePool>,
    form: web::Json<CreateTask>,
) -> Result<impl Responder, AppError> {
    let res = sqlx::query("INSERT INTO tasks (name, description) VALUES (?, ?)")
        .bind(&form.name)
        .bind(form.description.as_deref())
        .execute(db_pool.get_ref())
        .await
        .map_err(AppError::Database)?;

    let id = res.last_insert_rowid();
    let new_task = Task {
        id,
        name: form.name.clone(),
        description: form.description.clone()
    };

    Ok(HttpResponse::Created().json(new_task))
}

#[delete("/tasks/{id}")]
async fn delete_task(
    db_pool: web::Data<SqlitePool>,
    path: web::Path<i64>,
) -> Result<impl Responder, AppError> {
    let id = path.into_inner();
    let result = sqlx::query("DELETE FROM tasks WHERE id = ?")
        .bind(id)
        .execute(db_pool.get_ref())
        .await
        .map_err(AppError::Database)?;

    if result.rows_affected() == 0 {
        Err(AppError::NotFound(format!("Task {} not found", id)))
    } else {
        Ok(HttpResponse::Ok().body(format!("Deleted task {}", id)))
    }
}

#[put("tasks/{id}")]
async fn update_task(
    db_pool: web::Data<SqlitePool>,
    path: web::Path<i64>,
    form: web::Json<CreateTask>,
    ) -> Result <impl Responder, AppError> {
    let id = path.into_inner();
    let result = sqlx::query("UPDATE tasks SET name = ?, description = ? WHERE id = ?")
        .bind(&form.name)
        .bind(form.description.as_deref())
        .bind(id)
        .execute(db_pool.get_ref())
        .await
        .map_err(AppError::Database)?;
    if result.rows_affected() == 0 {
        Err(AppError::NotFound(format!("Task {} not found", id)))
    }else{
        Ok(HttpResponse::Ok().body(format!("updated task {}", id)))
    }

}

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    dotenv().ok();
    let db_url = env::var("DATABASE_URL").expect("DATABASE_URL must be set in .env");

    // This will create the database file if it doesn't exist.
    let db_pool = SqlitePool::connect(&db_url)
        .await
        .expect("Failed to connect to SQLite");

    // --- Pure Rust Database Scaffolding ---
    // This runs the schema creation query automatically on startup.
    // 'CREATE TABLE IF NOT EXISTS' makes it safe to run every time.
    sqlx::query(
        "CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT
        );",
    )
    .execute(&db_pool)
    .await
    .expect("Failed to create tasks table.");

    println!("Server running at http://127.0.0.1:8080");

    HttpServer::new(move || {
        App::new()
            .app_data(web::Data::new(db_pool.clone()))
            .service(get_tasks)
            .service(get_task_by_id)
            .service(create_task)
            .service(delete_task)
            .service(update_task)
    })
    .bind(("127.0.0.1", 8080))?
    .run()
    .await
}
