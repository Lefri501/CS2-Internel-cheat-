# Design: Project Analysis and Development Readiness

## 1. Purpose and Scope

This feature produces a documentation-only readiness baseline for the Lefrizzel-Ai repository. It establishes an evidence-backed reference for later fixes, improvements, and additions without changing production code, project configuration, SDK material, or other runtime inputs.

The design treats the repository as the source of truth for implementation and build facts. Existing documentation, SDK dumps, and analysis databases are additional evidence sources and are recorded separately when they agree or conflict.

### In scope

- Repository and artifact inventory.
- Visual Studio/C++23 build baseline.
- DLL runtime initialization sequence.
- Subsystem and integration boundaries.
- External dependency inventory.
- Local CS2 SDK and reverse-engineering resource map.
- IDA Pro database availability and evidence workflow.
- Verified facts, unverified statements, development gaps, and recommended next inspections.
- Preservation verification for production code and project build configuration.

### Out of scope

- Production-code changes.
- Project-file or solution-file changes.
- Offset, pattern, interface, or function invention when source evidence is absent.
- Rebuilding or modifying the CS2 game installation.
- Claiming runtime behavior that cannot be established from inspected source/configuration/resources.

## 2. Architecture

The baseline is produced as a deterministic inspection pipeline with a documentation boundary:

```text
Repository filesystem and project files
        |
        v
Evidence collectors
  - repository inventory
  - build configuration
  - source/runtime map
  - dependency/integration scan
  - SDK/IDA resource scan
        |
        v
Evidence normalizer and conflict detector
        |
        v
Readiness baseline renderer
        |
        v
.kiro/specs/project-analysis-development-readiness/readiness-baseline.md
```

The pipeline is read-only with respect to the repository under analysis. The only intended write is the generated documentation artifact in the spec directory. If the workflow uses a temporary working copy or fixture, that copy is disposable and must not be treated as a production change.

### Evidence precedence

1. Direct source or project configuration evidence.
2. Local SDK/resource files that can be inspected directly.
3. Existing project documentation.
4. Inference, which is not sufficient for a verified fact and must be labeled unverified.

When sources disagree, the renderer retains both citations and emits a development gap instead of selecting one silently.

## 3. Components

### 3.1 Repository Inventory Collector

Identifies and classifies repository paths into the required categories:

- Source: C++ implementation and headers.
- Configuration: solution, project files, property sheets, build scripts, and relevant configuration files.
- Documentation: `docs`, project notes, and analysis documents.
- External dependency: vendored libraries such as ImGui and other third-party code.
- SDK material: local CS2 dump files and generated bindings.
- Generated artifact: binaries, intermediate directories, logs, databases, or other build outputs.
- Unknown: paths that cannot be classified from available evidence.

Required inventory anchors are recorded as separate entries: solution, primary C++ project, documentation directory, SDK directory, external dependency directory, and generated/build-artifact directories.

### 3.2 Build Baseline Collector

Reads the solution and project configuration and records:

- C++ language standard.
- Visual Studio toolset.
- Windows SDK version.
- Target architecture/platform.
- Target type and output artifact.
- Runtime library.
- Preprocessor definitions.
- Linker libraries and relevant linker settings.
- Output/intermediate directories.
- Reproducible solution entry point and configuration-platform pair.

Each value includes an evidence citation. Missing values are represented as unresolved rather than inferred. Values that differ between project configurations are represented per configuration and also produce a development gap when the difference affects the stated baseline.

### 3.3 Runtime and Subsystem Mapper

Reads the primary source files and project configuration to produce a source-supported execution map. The runtime sequence records, where present:

1. DLL entry point.
2. Manual-map or loader detection.
3. Startup thread creation.
4. Required module/readiness waits.
5. Renderer/backend setup.
6. Configuration and logging setup.
7. Feature/subsystem initialization.
8. Hook installation and activation.
9. Shutdown or cleanup path when observable.

The mapper also creates feature-boundary records for each identified subsystem. A boundary describes the subsystem name, source location, owned responsibility, external inputs, and integration points. The mapper records ordering and dependencies but does not claim that a call exists unless it is supported by source or project evidence.

### 3.4 Dependency and Integration Collector

Combines project references, include paths, linker inputs, vendored directories, and source usage to identify dependencies and mechanisms. Each dependency record contains:

- Dependency name.
- Repository location or external-reference location.
- Observed integration role.
- Build relationship.
- Runtime relationship.
- Consuming subsystem.
- Evidence citation.

The collector explicitly maps hooks, schema bindings, pattern scanners, renderer backends, configuration loaders, and logging facilities to their source locations and consuming subsystems. A source reference without a local implementation or build reference becomes an unresolved dependency gap.

### 3.5 CS2 Resource Collector

Inventories the local SDK dump and records the required categories:

- Patterns: `patterns.hpp` and `patterns.json`.
- Offsets.
- Schemas.
- Interfaces.
- Protobuf material when present.
- Aggregate SDK include/material when present.

For each named pattern referenced by project code or documentation, the baseline records the pattern name, source file, available signature representation, target module, and whether the module is unresolved. No signature or target module is invented when it cannot be established.

### 3.6 IDA Resource Collector

Checks the documented Counter-Strike 2 installation locations for available `.i64` databases and records each available database with its associated module. The baseline distinguishes:

- Available and inspected resource.
- Referenced but unavailable resource.
- Expected path that was not accessible.
- Resource whose module association is unresolved.

The documented reverse-engineering workflow is:

1. Look up the named pattern in the local `patterns.hpp` or `patterns.json`.
2. Locate the corresponding signature in the matching module database.
3. Analyze the discovered function or reference.
4. Decompile only after the target has been identified.
5. Record the evidence and unresolved assumptions for later work.

The workflow is documentation only; it does not fabricate IDA findings or require an unavailable database.

### 3.7 Baseline Renderer

Renders a concise, bullet-oriented Markdown document with the following sections:

- Repository layout and inventory.
- Exact build baseline.
- Runtime initialization.
- Feature boundaries.
- External dependencies and integrations.
- CS2 SDK resources.
- IDA Pro databases.
- Reverse-engineering workflow.
- Verified facts.
- Unverified statements.
- Development gaps.
- Recommended inspection targets.
- Preservation result and analysis boundary.

Every substantive claim is either accompanied by an evidence citation or placed in the unverified/gap section.

## 4. Interfaces

The implementation may be manual analysis or tooling-assisted, but the logical interfaces are:

### Evidence record

```text
EvidenceRecord {
    id: string
    category: Inventory | Build | Runtime | Boundary | Dependency | SDK | IDA | Documentation
    subject: string
    claim: string
    source_path: string
    source_location: string | null
    status: Verified | Unverified | Conflicting | Missing
    related_gap_id: string | null
}
```

### Inventory entry

```text
InventoryEntry {
    path: string
    classification: Source | Configuration | Documentation |
                   ExternalDependency | SDKMaterial | GeneratedArtifact | Unknown
    existence: Present | Missing | Inaccessible
    evidence: EvidenceRecord[]
}
```

### Build setting

```text
BuildSetting {
    name: string
    value: string | Unresolved
    configuration_platform: string
    source_path: string
    status: Verified | Conflicting | Unresolved
    gap_id: string | null
}
```

### Feature boundary

```text
FeatureBoundary {
    name: string
    source_locations: string[]
    responsibility: string
    external_inputs: string[]
    integration_points: string[]
    status: Verified | Unverified
    gap_id: string | null
}
```

### Development gap

```text
DevelopmentGap {
    id: string
    category: Inventory | Build | Runtime | Boundary |
              Dependency | SDK | IDA | Conflict | Preservation
    statement: string
    affected_paths_or_resources: string[]
    evidence: EvidenceRecord[]
    recommended_next_inspection: string
}
```

The renderer consumes these records and does not alter their factual status. In particular, unresolved values remain unresolved in the final document.

## 5. Data and Document Model

The generated baseline is a Markdown evidence report. It should use compact tables where a fixed schema improves scanning and bullets where findings or recommendations are more readable.

Each section follows the same pattern:

- Finding or fact.
- Evidence path and, where available, line/configuration context.
- Status: verified, unverified, conflicting, or missing.
- Gap reference when status is not verified.

The report separates three classes of statements:

- **Verified facts:** directly supported by inspected repository files/resources.
- **Unverified statements:** plausible or documented claims lacking sufficient direct evidence.
- **Development gaps:** actionable missing, stale, contradictory, inaccessible, or unresolved evidence.

The preservation section records that production source and project build configuration were not changed. If a before/after comparison is available, it records the comparison method and result; otherwise it marks the preservation check as requiring confirmation rather than claiming success.

## 6. Error Handling and Evidence Rules

- Missing paths are recorded with their exact expected path and `Missing` or `Inaccessible` status.
- A required field absent from a project file is recorded as `Unresolved` and creates a build gap.
- Conflicting project and documentation claims retain both source references and create a conflict gap.
- A referenced dependency without an identifiable implementation or build reference creates a dependency gap naming the dependency.
- A runtime step or subsystem boundary not supported by source/configuration is labeled unverified and creates a gap.
- An unavailable SDK or IDA item creates a gap without inventing signatures, offsets, paths, module associations, or function behavior.
- Duplicate evidence is consolidated by subject and source while preserving all materially different claims.
- Analysis failures do not abort the entire baseline when another section can still be inspected; the failure is captured in the affected section and gap list.
- The analysis must not write to production source directories, project files, solution files, SDK directories, game-installation resources, or generated runtime inputs.

## 7. Verification Strategy

### Example and document-structure checks

- Required repository anchors appear as separate inventory entries with allowed classifications.
- Required build fields and evidence sources appear in the build section.
- Runtime steps are listed in source-supported order.
- Each feature boundary includes all required fields.
- Each dependency and integration mechanism has location, role, and consumer information.
- SDK categories and available IDA databases are mapped with explicit availability status.
- The baseline contains verified facts, unverified statements, gaps, and recommended inspection targets.
- Required sections use concise bullets/tables and do not contain fabricated evidence.

### Edge-case checks

- Missing or inaccessible inventory paths become gaps.
- Missing or inconsistent build settings become unresolved values and gaps.
- Unverifiable runtime/boundary claims become unverified findings and gaps.
- Referenced but unresolved dependencies become named dependency gaps.
- Missing SDK/IDA resources become named gaps without invented technical details.
- Conflicting source and documentation claims retain both sources and become conflict gaps.

### Smoke checks

- The solution and configuration-platform entry point are present and documented.
- Available `.i64` databases are associated with modules where the association can be verified.
- Repository status or an equivalent before/after comparison confirms that production code and build configuration remain unchanged.

Property-based testing is not required for this feature. The core inputs are repository artifacts, external analysis resources, and configuration files; broad randomized generation would not provide more assurance than evidence-focused examples, controlled missing-resource fixtures, and preservation checks.

## 8. Correctness Properties

*A correctness property is a behavior that must hold for every valid analysis result, independent of the particular repository path or subsystem being documented.*

### Property 1: Evidence status is preserved

For any evidence record, the rendered baseline SHALL preserve its source path and status, and SHALL NOT render an unverified, conflicting, or missing claim as a verified fact.

**Validates: Requirements 1.2, 2.3, 3.3, 5.4, 6.1, 6.4**

### Property 2: Required inventory anchors remain distinct and classified

For all readiness baselines, each required repository anchor SHALL appear as a separate inventory entry with exactly one allowed classification and an explicit present, missing, or inaccessible state.

**Validates: Requirements 1.1, 1.2, 1.3**

### Property 3: Build claims are evidence-complete or explicitly unresolved

For any build configuration represented in the baseline, every required build field SHALL have an evidence-backed value; when the field is absent or inconsistent, the field SHALL be marked unresolved and linked to a development gap rather than inferred.

**Validates: Requirements 2.1, 2.3**

### Property 4: Runtime and boundary claims are source-supported

For all runtime steps and feature-boundary records marked verified, the baseline SHALL include a source or project-configuration citation for the claim and all required boundary fields; unsupported claims SHALL be marked unverified and linked to a gap.

**Validates: Requirements 3.1, 3.2, 3.3**

### Property 5: Integration records identify their consumer

For any recorded external dependency or integration mechanism, the baseline SHALL include its location, observed role, and build or runtime relationship, and SHALL name the consuming subsystem or mark that relationship unresolved with a development gap.

**Validates: Requirements 4.1, 4.2, 4.3**

### Property 6: Reverse-engineering references never gain invented details

For all SDK pattern references and IDA resource records, each available detail SHALL be traceable to an inspected local resource; an unavailable target, database, signature, offset, or module association SHALL remain explicitly unresolved and SHALL produce a development gap.

**Validates: Requirements 5.1, 5.2, 5.3, 5.4**

### Property 7: Conflicts are lossless and actionable

For any conflicting pair of project and documentation claims, the baseline SHALL retain both evidence sources, identify the conflict, classify the affected statement as a development gap, and provide a recommended next inspection.

**Validates: Requirements 6.1, 6.4**

### Property 8: Analysis preserves the production boundary

For all completed analyses, production source files, SDK integration files, solution/project build configuration, and runtime configuration SHALL have identical contents before and after analysis; only the designated documentation artifact may be added or updated.

**Validates: Requirement 6.3**

## 9. Recommended Output Contract

The implementation phase should create the readiness baseline at:

```text
.kiro/specs/project-analysis-development-readiness/readiness-baseline.md
```

The document must be concise and bullet-oriented, but should retain enough evidence paths to make every later development decision auditable. No production project file or source file is part of this feature’s output.
