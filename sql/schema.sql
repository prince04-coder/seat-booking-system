-- ============================================================
-- Ticketing Engine — MySQL 8.0 Schema
-- ============================================================

CREATE TABLE IF NOT EXISTS events (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    name        VARCHAR(255) NOT NULL,
    venue       VARCHAR(255),
    event_date  DATETIME,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS seats (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    event_id    INT NOT NULL,
    seat_label  VARCHAR(10) NOT NULL,
    is_booked   TINYINT(1) NOT NULL DEFAULT 0,
    booked_by   VARCHAR(255),
    booked_at   DATETIME,
    UNIQUE KEY unique_seat (event_id, seat_label),
    INDEX idx_event (event_id),
    FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ============================================================
-- Seed Data
-- ============================================================
INSERT IGNORE INTO events (id, name, venue, event_date)
VALUES (1, 'Grand Tech Conference 2026', 'Convention Center', '2026-09-15 18:00:00');

-- 50 seats: A1-A10 ... E1-E10 using recursive CTE (MySQL 8.0+)
INSERT IGNORE INTO seats (event_id, seat_label)
WITH RECURSIVE nums AS (
    SELECT 0 AS n
    UNION ALL
    SELECT n + 1 FROM nums WHERE n < 49
)
SELECT 1,
       CONCAT(CHAR(65 + (n DIV 10) USING utf8mb4),
              CAST((n MOD 10) + 1 AS CHAR))
FROM nums;
