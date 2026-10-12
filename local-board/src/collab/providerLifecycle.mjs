/** Release the provider and the dedicated WebSocket created for this board. */
export function destroyOwnedProvider(provider) {
  const socket = provider.configuration.websocketProvider;
  try {
    provider.destroy();
  } finally {
    socket.destroy();
  }
}
