<?php

/**
 * SQL Server-specific implementation of INSERT.
 *
 * SQL Server doesn't supports multi-insert queries, and needs special handling for
 * binary columns.
 */
class InsertQuery_sqlsrv extends InsertQuery {

  public function execute() {
    if (!$this->preExecute()) {
      return NULL;
    }

    // Fetch the list of blobs and sequences used on that table.
    $columnInformation = $this->connection->schema()->queryColumnInformation($this->table);

    // Find out if there is an identity field set in this insert.
    foreach ($this->insertFields as $field) {
      if (isset($columnInformation['identities'][$field])) {
        $this->setIdentity = TRUE;
      }
    }

    // Each insert happens in its own query. However, we wrap it in a transaction
    // so that it is atomic where possible.
    if (empty($this->queryOptions['sqlsrv_skip_transactions'])) {
      $transaction = $this->connection->startTransaction();
    }

    if (!empty($this->fromQuery)) {
      // Re-initialize the values array so that we can re-use this query.
      $this->insertValues = array();

      $stmt = $this->connection->PDOPrepare($this->connection->prefixTables((string) $this));
      // Handle the case of SELECT-based INSERT queries first.
      foreach ($this->fromQuery->getArguments() as $key => $value) {
        $stmt->bindParam($key, $value);
      }

      try {
        $stmt->execute();
      }
      catch (Exception $e) {
        // This INSERT query failed, rollback everything if we started a transaction earlier.
        if (!empty($transaction)) {
          $transaction->rollback();
        }
        // Rethrow the exception.
        throw $e;
      }

      return $this->connection->lastInsertId();
    }

    // Handle the case of full-default queries.
    if (empty($this->fromQuery) && (empty($this->insertFields) || empty($this->insertValues))) {
      // Re-initialize the values array so that we can re-use this query.
      $this->insertValues = array();

      $stmt = $this->connection->PDOPrepare($this->connection->prefixTables('INSERT INTO {' . $this->table . '} DEFAULT VALUES'));
      try {
        $stmt->execute();
      }
      catch (Exception $e) {
        // This INSERT query failed, rollback everything if we started a transaction earlier.
        if (!empty($transaction)) {
          $transaction->rollback();
        }
        // Rethrow the exception.
        throw $e;
      }

      return $this->connection->lastInsertId();
    }

    $query = (string) $this;
    $stmt = $this->connection->PDOPrepare($this->connection->prefixTables($query));

    // We use this array to store references to the blob handles.
    // This is necessary because the PDO will otherwise messes up with references.
    $blobs = array();

    foreach ($this->insertValues as $insert_values) {
      $max_placeholder = 0;
      foreach ($this->insertFields as $idx => $field) {
        $placeholder = ':db_insert' . $max_placeholder++;
        if (isset($columnInformation['blobs'][$field])) {
          $blob_handle = fopen('php://memory', 'a');
          fwrite($blob_handle, $insert_values[$idx]);
          rewind($blob_handle);
          $blobs[$placeholder] = $blob_handle;

          $stmt->bindParam($placeholder, $blobs[$placeholder], PDO::PARAM_LOB, 0, PDO::SQLSRV_ENCODING_BINARY);
        }
        else {
          $stmt->bindParam($placeholder, $insert_values[$idx]);
        }
      }

      try {
        $stmt->execute();
      }
      catch (Exception $e) {
        // This INSERT query failed, rollback everything if we started a transaction earlier.
        if (!empty($transaction)) {
          $transaction->rollback();
        }
        // Rethrow the exception.
        throw $e;
      }

      $last_insert_id = $this->connection->lastInsertId();
    }

    // Re-initialize the values array so that we can re-use this query.
    $this->insertValues = array();

    return $last_insert_id;
  }

  public function __toString() {
    // Enable direct insertion to identity columns if necessary.
    if (!empty($this->setIdentity)) {
      $prefix = 'SET IDENTITY_INSERT {' . $this->table . '} ON;';
    }
    else {
      $prefix = '';
    }

    // If we're selecting from a SelectQuery, finish building the query and
    // pass it back, as any remaining options are irrelevant.
    if (!empty($this->fromQuery)) {
      return $prefix . "INSERT INTO {" . $this->table . '} (' . implode(', ', $this->connection->quoteIdentifiers($this->insertFields)) . ') ' . $this->fromQuery;
    }

    // Build the list of placeholders.
    $placeholders = array();
    for ($i = 0; $i < count($this->insertFields); ++$i) {
      $placeholders[] = ':db_insert' . $i;
    }

    return $prefix . 'INSERT INTO {' . $this->table . '} (' . implode(', ', $this->connection->quoteIdentifiers($this->insertFields)) . ') VALUES (' . implode(', ', $placeholders) . ')';
  }
}

class UpdateQuery_sqlsrv extends UpdateQuery {
  public function execute() {
    $max_placeholder = 0;

    // Because we filter $fields the same way here and in __toString(), the
    // placeholders will all match up properly.
    $stmt = $this->connection->PDOPrepare($this->connection->prefixTables((string)$this));

    // Fetch the list of blobs and sequences used on that table.
    $columnInformation = $this->connection->schema()->queryColumnInformation($this->table);

    // Expressions take priority over literal fields, so we process those first
    // and remove any literal fields that conflict.
    $fields = $this->fields;
    $expression_fields = array();
    foreach ($this->expressionFields as $field => $data) {
      if (!empty($data['arguments'])) {
        foreach ($data['arguments'] as $placeholder => $argument) {
          // We assume that an expression will never happen on a BLOB field,
          // which is a fairly safe assumption to make since in most cases
          // it would be an invalid query anyway.
          $stmt->bindParam($placeholder, $argument);
        }
      }
      unset($fields[$field]);
    }

    // We use this array to store references to the blob handles.
    // This is necessary because the PDO will otherwise messes up with references.
    $blobs = array();
    $blob_count = 0;

    foreach ($fields as $field => &$value) {
      $placeholder = ':db_update_placeholder_' . ($max_placeholder++);

      if (isset($columnInformation['blobs'][$field])) {
        $blobs[$blob_count] = fopen('php://memory', 'a');
        fwrite($blobs[$blob_count], $value);
        rewind($blobs[$blob_count]);
        $stmt->bindParam($placeholder, $blobs[$blob_count], PDO::PARAM_LOB, 0, PDO::SQLSRV_ENCODING_BINARY);
        $blob_count++;
      }
      else {
        $stmt->bindParam($placeholder, $value);
      }
    }

    if (count($this->condition)) {
      $this->condition->compile($this->connection, $this);

      $arguments = $this->condition->arguments();
      foreach ($arguments as $placeholder => &$value) {
        $stmt->bindParam($placeholder, $value);
      }
    }

    $options = $this->queryOptions;
    $options['already_prepared'] = TRUE;
    $stmt->execute();

    return $stmt->rowCount();
  }

  public function __toString() {
    // Expressions take priority over literal fields, so we process those first
    // and remove any literal fields that conflict.
    $fields = $this->fields;
    $update_fields = array();
    foreach ($this->expressionFields as $field => $data) {
      $update_fields[] = $this->connection->quoteIdentifier($field) . '=' . $data['expression'];
      unset($fields[$field]);
    }

    $max_placeholder = 0;
    foreach ($fields as $field => $value) {
      $update_fields[] = $this->connection->quoteIdentifier($field) . '=:db_update_placeholder_' . ($max_placeholder++);
    }

    $query = 'UPDATE {' . $this->connection->escapeTable($this->table) . '} SET ' . implode(', ', $update_fields);

    if (count($this->condition)) {
      $this->condition->compile($this->connection, $this);
      // There is an implicit string cast on $this->condition.
      $query .= "\nWHERE " . $this->condition;
    }

    return $query;
  }
}

/**
 * SQL Server-specific implementation of TRUNCATE.
 */
class TruncateQuery_sqlsrv extends TruncateQuery {
  public function __toString() {
    return 'TRUNCATE TABLE {' . $this->connection->escapeTable($this->table) . '} ';
  }
}

class MergeQuery_sqlsrv extends MergeQuery {
  public function execute() {
    // If validation fails, simply return NULL.
    // Note that validation routines in preExecute() may throw exceptions instead.
    if (!$this->preExecute()) {
      return NULL;
    }

    // In the degenerate case of this query type, we have to run multiple
    // queries as there is no universal single-query mechanism that will work.
    // Our degenerate case is not designed for performance efficiency but
    // for comprehensibility. Any practical database driver will override
    // this method with database-specific logic, so this function serves only
    // as a fallback to aid developers of new drivers.

    // Wrap multiple queries in a transaction, if the database supports it.
    $transaction = $this->connection->startTransaction();

    // We don't need INSERT or UPDATE queries to trigger additional transactions.
    $this->queryOptions['sqlsrv_skip_transactions'] = TRUE;

    try {
      // Try to insert first.
      $insert_fields = $this->insertFields + $this->keyFields;
      $this->connection->insert($this->table, $this->queryOptions)
        ->fields($insert_fields)
        ->execute();
      return MergeQuery::STATUS_INSERT;
    }
    catch (PDOException $e) {
      // Or else update.
      if ($this->updateFields) {
        $update_fields = $this->updateFields;
      }
      else {
        $update_fields = $this->insertFields;
        // If there are no exclude fields, this is a no-op.
        foreach ($this->excludeFields as $exclude_field) {
          unset($update_fields[$exclude_field]);
        }
      }
      if ($update_fields || $this->expressionFields) {
        // Only run the update if there are no fields or expressions to update.
        $update = $this->connection->update($this->table, $this->queryOptions)->fields($update_fields);
        foreach ($this->keyFields as $field => $value) {
          $update->condition($field, $value);
        }
        foreach ($this->expressionFields as $field => $expression) {
          $update->expression($field, $expression['expression'], $expression['arguments']);
        }
        $update->debug = TRUE;
        $update->execute();
        return MergeQuery::STATUS_UPDATE;
      }
    }

    // Transaction commits here where $transaction looses scope.
  }
}