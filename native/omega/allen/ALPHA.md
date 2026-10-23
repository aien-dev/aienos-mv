# Allen alpha delivery contract

User direction: Allen is a sovereign stack, not a mandatory client of Drake's
Spark. The initial visual release targets the iMac. One shared handwritten
Omega core supports native boot and an optional hosted application experience.
These launch modes are qualified separately; current Linux AArch64 module tests
do not qualify an iMac boot image or a macOS application.

## Free, temporary hosted experience

The hosted app is a free, temporary way to experience the interface and speed
before choosing a native installation. It must leave boot configuration,
partitions and drivers untouched, and uninstall cleanly. It is not meant to
replace the sovereign stack with a permanent dependency on another OS or service.
The hosted trial necessarily uses its host OS interfaces; native boot remains
the path where Allen owns boot/kernel/device behavior.

## User-controlled inference

- Local: use supported compute on the user's own machine through AIEN's engine.
  This is a first-class path, not gated on an AIEN account or remote API key.
- User cloud: use compute the user provisions and controls, through AIEN's stack.
- Drake's Spark: optional hosted inference requiring an API key for that service.

No mandatory Spark service, cloud provider, vendor runtime or subscription is
introduced by these choices. Unsupported local hardware is reported honestly,
not silently substituted with remote inference. Backend selection and permission
to send data off-device must be explicit. Keys are never baked into a download
or logs. Do not ask users to paste keys into development chat.

Every displayed speed result must identify the inference location and relevant
hardware. A Spark-backed response is not evidence of the user's local speed.
A free app does not imply free third-party cloud infrastructure; any such cost
must be disclosed before the user selects it. No performance guarantee without
measurements on the actual named path.

## Public alpha acceptance

Publicly obtainable artifact, supported-platform instructions, source/license
provenance, checksums, clean acquisition outside a developer checkout, actual
visual conversation and persistence, and recovery/uninstall instructions.
Qualify one bundled native iMac trial separately from the hosted safe trial.
Unknown hardware gets detection and bounded capability checks, not guessed writes.

The font, provider and retained-presentation components are implemented. The
bounded RGB surface is being qualified. Live backend binding, hosted window/input
and platform packaging, native boot/device integration and physical iMac acceptance
remain unfinished. Do not label component images or protocol fixtures a released
interactive alpha.
