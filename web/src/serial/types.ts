/**
 * Wire types for MeshyQ's structured JSON control protocol (firmware/src/web_api.c).
 * Web Serial's own DOM types (SerialPort, navigator.serial, ...) come from
 * the @types/w3c-web-serial package and don't need to be redeclared here.
 */

export interface WebApiArgs {
  filename?: string;
  text?: string;
  presence?: string;
  addr?: number;
  percent?: number;
}

export interface WebApiRequest {
  v: number;
  id: number;
  cmd: string;
  args?: WebApiArgs;
}

export interface WebApiError {
  code: string;
  message: string;
}

export interface WebApiResponse {
  v: number;
  id: number;
  ok: boolean;
  result?: Record<string, unknown>;
  error?: WebApiError;
}

export interface WebApiEvent {
  v: number;
  event: string;
  addr?: string;
  text?: string;
  presence?: string;
}

export type WebApiMessage = WebApiResponse | WebApiEvent;

export function isWebApiEvent(msg: WebApiMessage): msg is WebApiEvent {
  return "event" in msg;
}
