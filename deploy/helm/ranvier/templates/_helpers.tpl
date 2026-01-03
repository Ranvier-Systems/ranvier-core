{{/*
Expand the name of the chart.
*/}}
{{- define "ranvier.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
We truncate at 63 chars because some Kubernetes name fields are limited to this (by the DNS naming spec).
If release name contains chart name it will be used as a full name.
*/}}
{{- define "ranvier.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Create chart name and version as used by the chart label.
*/}}
{{- define "ranvier.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels
*/}}
{{- define "ranvier.labels" -}}
helm.sh/chart: {{ include "ranvier.chart" . }}
{{ include "ranvier.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels
*/}}
{{- define "ranvier.selectorLabels" -}}
app.kubernetes.io/name: {{ include "ranvier.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Create the name of the service account to use
*/}}
{{- define "ranvier.serviceAccountName" -}}
{{- if .Values.serviceAccount.create }}
{{- default (include "ranvier.fullname" .) .Values.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.serviceAccount.name }}
{{- end }}
{{- end }}

{{/*
Create the name of the headless service for peer discovery
*/}}
{{- define "ranvier.headlessServiceName" -}}
{{- printf "%s-headless" (include "ranvier.fullname" .) }}
{{- end }}

{{/*
Create the DNS name for gossip peer discovery
*/}}
{{- define "ranvier.gossipDnsName" -}}
{{- printf "%s.%s.svc.cluster.local" (include "ranvier.headlessServiceName" .) .Release.Namespace }}
{{- end }}

{{/*
Create the image name
*/}}
{{- define "ranvier.image" -}}
{{- $tag := default .Chart.AppVersion .Values.image.tag }}
{{- printf "%s:%s" .Values.image.repository $tag }}
{{- end }}

{{/*
Create the secret name for API keys
*/}}
{{- define "ranvier.secretName" -}}
{{- if .Values.auth.existingSecret }}
{{- .Values.auth.existingSecret }}
{{- else }}
{{- printf "%s-api-keys" (include "ranvier.fullname" .) }}
{{- end }}
{{- end }}

{{/*
Create the configmap name
*/}}
{{- define "ranvier.configMapName" -}}
{{- printf "%s-config" (include "ranvier.fullname" .) }}
{{- end }}

{{/*
Backend discovery namespace
*/}}
{{- define "ranvier.backendNamespace" -}}
{{- if .Values.backends.discovery.namespace }}
{{- .Values.backends.discovery.namespace }}
{{- else }}
{{- .Release.Namespace }}
{{- end }}
{{- end }}

{{/*
Generate Seastar command line arguments
*/}}
{{- define "ranvier.seastarArgs" -}}
--smp {{ .Values.seastar.smp }} --memory {{ .Values.seastar.memory }}
{{- range .Values.seastar.extraArgs }} {{ . }}{{- end }}
{{- end }}
