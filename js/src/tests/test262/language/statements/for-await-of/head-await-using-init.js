// |reftest| shell-option(--enable-explicit-resource-management) skip-if(!(this.hasOwnProperty('getBuildConfiguration')&&getBuildConfiguration('explicit-resource-management'))||!xulRuntime.shell) error:SyntaxError -- explicit-resource-management is not enabled unconditionally, requires shell-options
// Copyright (C) 2023 Ron Buckton. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-iteration-statements
description: >
  ForDeclaration containing 'await using' does not allow initializer.
info: |
  IterationStatement:
    for await (ForDeclaration of AssignmentExpression) Statement
negative:
  phase: parse
  type: SyntaxError
features: [async-iteration, explicit-resource-management]
---*/

$DONOTEVALUATE();

async function fn() {
  const obj = { async [Symbol.asyncDispose]() {} };
  for await (await using x = obj of []) {}
}
